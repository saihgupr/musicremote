/*
 * musicremote sends one command to Music.app through the MusicRemote tweak:
 * write req.json {"id","action","args"} (tmp+fsync+rename), post the Darwin
 * notification com.chris.musicremote.req, poll res.json every 50 ms for 3 s
 * for a response carrying our id. An exclusive flock serialises calls;
 * req.json is removed on exit, res.json is left for the next call to overwrite.
 * No answer after one retry (launching Music.app): exit 2.
 *
 * Album, artist and song names are resolved in MediaLibrary.sqlitedb (opened
 * read-only) and queued as Apple Music catalog IDs through queueByStore, with
 * a queueByID fallback when a track has no store ID. library and search read
 * the same database; queue album/artist take their IDs from a status reply.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_PATH        "/var/mobile/Media/.musicremote"
#define DB_PATH         "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb"
#define NOTIFY_REQ      "com.chris.musicremote.req"
#define LOCK_WAIT_MS    2000
#define POLL_MS         50
#define ATTEMPT_MS      3000
#define MAX_ATTEMPTS    3
#define DEFAULT_LIMIT   20
#define SEARCH_MAX      50
#define MAX_QUERY       400
#define MAX_CAND        64
#define MAX_STORE_IDS   600
#define SUGGEST_MAX     5
#define MAX_SQL         1536
#define MAX_FILE_BYTES  (1 << 20)

enum { RESP_NONE = 0, RESP_OK, RESP_ERR };

typedef struct { char buf[4096]; size_t len; } sbuf;
typedef struct { const char *action, *args; } request_t;

/* --json is set once from main; local failures use the same {"error":..}
 * shape the tweak's error responses get. */
static int opt_json;

static void oom(void) { fputs("out of memory\n", stderr); exit(1); }

/* ---------- paths ---------- */

static char dir_path[PATH_MAX];
static char req_path[PATH_MAX];
static char tmp_path[PATH_MAX];
static char res_path[PATH_MAX];
static char lock_path[PATH_MAX];

/* MUSICREMOTE_DIR and MUSICREMOTE_DB override the device paths for host
 * tests; on device the defaults above are used. */
static void init_paths(void) {
	const char *d = getenv("MUSICREMOTE_DIR");
	snprintf(dir_path, sizeof dir_path, "%s", d && *d ? d : DIR_PATH);
	snprintf(req_path, sizeof req_path, "%s/req.json", dir_path);
	snprintf(tmp_path, sizeof tmp_path, "%s/req.json.tmp", dir_path);
	snprintf(res_path, sizeof res_path, "%s/res.json", dir_path);
	snprintf(lock_path, sizeof lock_path, "%s/lock", dir_path);
}

/* ---------- request JSON ---------- */

static int sb_addf(sbuf *s, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(s->buf + s->len, sizeof s->buf - s->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof s->buf - s->len) return -1;
	s->len += (size_t)n;
	return 0;
}

/* Appends a JSON string literal; -1 if it holds a control character. */
static int sb_add_escaped(sbuf *s, const char *v) {
	if (sb_addf(s, "\"") < 0) return -1;
	for (; *v; v++) {
		unsigned char c = (unsigned char)*v;
		if (c < 0x20) return -1;
		if ((c == '"' || c == '\\') && sb_addf(s, "\\") < 0) return -1;
		if (sb_addf(s, "%c", (char)c) < 0) return -1;
	}
	return sb_addf(s, "\"");
}

static int arg_str(sbuf *a, const char *key, const char *val) {
	if (sb_addf(a, "%s\"%s\":", a->len > 1 ? "," : "", key) < 0) return -1;
	return sb_add_escaped(a, val);
}

static void arg_num(sbuf *a, const char *key, double v) {
	sb_addf(a, "%s\"%s\":%g", a->len > 1 ? "," : "", key, v);
}

/* Growable string, for the queueByStore id list: a 600-track artist does not
 * fit in sbuf. Freed by the caller. */
typedef struct { char *buf; size_t len, cap; } dbuf;

static void dbuf_reserve(dbuf *d, size_t extra) {
	if (d->len + extra + 1 <= d->cap) return;
	size_t cap = d->cap ? d->cap : 256;
	while (cap < d->len + extra + 1) cap *= 2;
	char *p = realloc(d->buf, cap);
	if (!p) oom();
	d->buf = p;
	d->cap = cap;
}

static void dbuf_add(dbuf *d, const char *s) {
	size_t n = strlen(s);
	dbuf_reserve(d, n);
	memcpy(d->buf + d->len, s, n);
	d->len += n;
	d->buf[d->len] = '\0';
}

/* Appends a word to a space-separated query; -1 if it does not fit. */
static int query_add(char *q, size_t n, const char *word) {
	size_t used = strlen(q), add = strlen(word) + (used ? 1 : 0);
	if (used + add + 1 > n) return -1;
	if (used) q[used++] = ' ';
	snprintf(q + used, n - used, "%s", word);
	return 0;
}

/* ---------- files and lock ---------- */

static int mkdir_p(const char *path) {
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1;; p++) {
		if (*p && *p != '/') continue;
		char c = *p;
		*p = '\0';
		int ok = mkdir(tmp, 0755) == 0 || errno == EEXIST;
		*p = c;
		if (!ok || !c) return ok ? 0 : -1;
	}
}

static int write_atomic(const char *path, const char *tmp, const char *data, size_t len) {
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644), ok = fd >= 0;
	for (size_t off = 0; ok && off < len;) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) ok = 0;
		else off += (size_t)n;
	}
	if (ok) ok = fsync(fd) == 0;
	if (fd >= 0 && close(fd) != 0) ok = 0;
	if (ok && rename(tmp, path) == 0) return 0;
	unlink(tmp);
	return -1;
}

static char resbuf[MAX_FILE_BYTES + 1];

static char *read_response(void) {
	int fd = open(res_path, O_RDONLY);
	if (fd < 0) return NULL;
	size_t n = 0;
	for (ssize_t r; n < MAX_FILE_BYTES; n += (size_t)r) {
		r = read(fd, resbuf + n, MAX_FILE_BYTES - n);
		if (r < 0 && errno == EINTR) { r = 0; continue; }
		if (r <= 0) break;
	}
	close(fd);
	if (n == 0) return NULL;
	resbuf[n] = '\0';
	return resbuf;
}

/* Exclusive flock with a 2 s retry; the fd stays open until exit. It is reused
 * for every request in this process, so current's status+queue pair stays one
 * critical section instead of fighting itself for the lock. */
static int lock_fd = -1;

static int take_lock(void) {
	if (lock_fd >= 0) return lock_fd;
	int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
	if (fd < 0) return -1;
	for (int waited = 0;; waited += POLL_MS) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
			lock_fd = fd;
			return fd;
		}
		if (errno != EWOULDBLOCK || waited >= LOCK_WAIT_MS) { close(fd); return -1; }
		usleep(POLL_MS * 1000);
	}
}

/* ---------- minimal JSON scanning ---------- */

static const char *skip_ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

/* p is at the opening quote; returns the byte after the closing quote. */
static const char *skip_string(const char *p) {
	for (p++; *p; p++) {
		if (*p == '\\' && p[1]) p++;
		else if (*p == '"') return p + 1;
	}
	return NULL;
}

static const char *skip_value(const char *p) {
	p = skip_ws(p);
	if (*p == '"') return skip_string(p);
	if (*p == '{' || *p == '[') {
		int d = 0;
		while (*p) {
			if (*p == '"') {
				if (!(p = skip_string(p))) return NULL;
				continue;
			}
			if (*p == '{' || *p == '[') d++;
			else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
			p++;
		}
		return NULL;
	}
	while (*p && !strchr(",}] \t\n\r", *p)) p++;
	return p;
}

/* Start of a top-level key's value, or NULL when absent. */
static const char *json_value(const char *obj, const char *key) {
	const char *p = skip_ws(obj);
	if (*p != '{') return NULL;
	for (p++;;) {
		p = skip_ws(p);
		if (*p != '"') return NULL;
		const char *ke = skip_string(p);
		if (!ke) return NULL;
		size_t klen = (size_t)(ke - p - 2);
		int hit = strlen(key) == klen && !strncmp(p + 1, key, klen);
		p = skip_ws(ke);
		if (*p != ':') return NULL;
		const char *ve = skip_value(skip_ws(p + 1));
		if (!ve) return NULL;
		if (hit) return skip_ws(p + 1);
		p = skip_ws(ve);
		if (*p != ',') return NULL;
		p++;
	}
}

/* ---------- JSON string values ---------- */

static void utf8_put(unsigned cp, char *o, size_t *n) {
	if (cp < 0x80) { o[(*n)++] = (char)cp; return; }
	if (cp < 0x800) {
		o[(*n)++] = (char)(0xC0 | (cp >> 6));
		o[(*n)++] = (char)(0x80 | (cp & 63));
		return;
	}
	o[(*n)++] = (char)(0xE0 | (cp >> 12));
	o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63));
	o[(*n)++] = (char)(0x80 | (cp & 63));
}

static void json_copy_string(const char *start, char *out, size_t outsz) {
	const char *end = skip_string(start), *p = start + 1;
	size_t n = 0;
	if (outsz == 0) return;
	/* Leave room for the terminator. Only an escape needs a reserve: a plain
	 * UTF-8 byte or a \\u escape costs at most 3 bytes. Reserving 5 for every
	 * character silently truncated 20-digit persistent IDs. */
	while (end && p < end - 1 && n + 1 < outsz) {
		if (*p != '\\') { out[n++] = *p++; continue; }
		unsigned cp;
		if (p[1] == 'u' && sscanf(p + 2, "%4x", &cp) == 1) {
			if (n + 3 >= outsz) break;
			p += 6;
			utf8_put(cp, out, &n);
			continue;
		}
		const char *e = strchr("\"\\/bfnrt", p[1]);
		out[n++] = e ? "\"\\/\b\f\n\r\t"[e - "\"\\/bfnrt"] : p[1];
		p += 2;
	}
	out[n] = '\0';
}

static int json_str(const char *obj, const char *key, char *out, size_t n) {
	const char *v = json_value(obj, key);
	if (!v || *v != '"') return 0;
	json_copy_string(v, out, n);
	return 1;
}

static int json_num(const char *obj, const char *key, double *out) {
	const char *v = json_value(obj, key);
	if (!v) return 0;
	char *end = NULL;
	double d = strtod(v, &end);
	if (end == v) return 0;
	*out = d;
	return 1;
}

static char *json_raw(const char *obj, const char *key, const char *def) {
	const char *v = json_value(obj, key), *e = v ? skip_value(v) : NULL;
	if (!v || !e) return strdup(def);
	char *p = malloc((size_t)(e - v) + 1);
	if (!p) oom();
	memcpy(p, v, (size_t)(e - v));
	p[e - v] = '\0';
	return p;
}

/* ---------- transport ---------- */

static int parse_response(const char *buf, const char *id, char **data, char **error) {
	char rid[64], msg[512];
	if (!json_str(buf, "id", rid, sizeof rid) || strcmp(rid, id)) return RESP_NONE;
	const char *ok = json_value(buf, "ok");
	if (ok && !strncmp(ok, "true", 4)) {
		*data = json_raw(buf, "data", "{}");
		return RESP_OK;
	}
	if (!ok || strncmp(ok, "false", 5)) return RESP_NONE;
	if (!json_str(buf, "error", msg, sizeof msg) || !msg[0])
		snprintf(msg, sizeof msg, "unknown error");
	if (!(*error = strdup(msg))) oom();
	return RESP_ERR;
}

static int poll_response(const char *id, char **data, char **error, int timeout_ms) {
	for (int waited = 0; waited <= timeout_ms; waited += POLL_MS) {
		char *buf = read_response();
		if (buf) {
			int r = parse_response(buf, id, data, error);
			if (r != RESP_NONE) return r;
		}
		if (waited == timeout_ms) break;
		usleep(POLL_MS * 1000);
	}
	return RESP_NONE;
}

/* ---------- human output ---------- */

static void fmt_time(double sec, char *out, size_t n) {
	int t = sec > 0 ? (int)sec : 0;
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void print_status(const char *data) {
	char state[32] = "", title[256] = "", artist[256] = "", album[256] = "", a[16], b[16];
	double elapsed = -1, duration = -1, index = -1, total = -1;
	json_str(data, "state", state, sizeof state);
	json_str(data, "title", title, sizeof title);
	json_str(data, "artist", artist, sizeof artist);
	json_str(data, "album", album, sizeof album);
	json_num(data, "elapsed", &elapsed);
	json_num(data, "duration", &duration);
	json_num(data, "index", &index);
	json_num(data, "total", &total);

	printf("%s", state[0] ? state : "unknown");
	if (title[0]) printf("  %s", title);
	if (artist[0]) printf(" - %s", artist);
	if (album[0]) printf("  [%s]", album);
	if (elapsed >= 0 && duration > 0) {
		fmt_time(elapsed, a, sizeof a);
		fmt_time(duration, b, sizeof b);
		printf("  %s / %s", a, b);
	}
	if (index >= 0 && total > 0) printf("  %d/%d", (int)index + 1, (int)total);
	putchar('\n');
}

static void print_search(const char *data) {
	double count = 0;
	const char *items = json_value(data, "items");
	if (json_num(data, "count", &count))
		printf("%d result%s\n", (int)count, count == 1.0 ? "" : "s");
	if (!items || *items != '[') return;
	const char *p = items + 1;
	for (int shown = 1;; shown++) {
		p = skip_ws(p);
		if (*p != '{') break;
		const char *end = skip_value(p);
		if (!end) break;
		char title[256] = "", artist[256] = "", album[256] = "";
		json_str(p, "title", title, sizeof title);
		json_str(p, "artist", artist, sizeof artist);
		json_str(p, "album", album, sizeof album);
		printf("%d. %s", shown, title[0] ? title : "(untitled)");
		if (artist[0]) printf(" - %s", artist);
		if (album[0]) printf("  [%s]", album);
		putchar('\n');
		p = skip_ws(end);
		if (*p != ',') break;
		p++;
	}
}

/* One extra line after a queueByID/queueByStore success: the player state the
 * tweak reported, when the response carries it. queueByID has no "queued"
 * count; queueByStore does. */
static void print_queue_state(const char *data) {
	char state[32] = "";
	double index = -1, total = -1;
	int has_state = json_str(data, "state", state, sizeof state);
	int has_index = json_num(data, "index", &index);
	int has_total = json_num(data, "total", &total);
	if (!has_state && !has_total) return;
	printf("%s", state[0] ? state : "unknown");
	if (has_index && has_total && total > 0) printf("  %d/%d", (int)index + 1, (int)total);
	else if (has_total) printf("  %d track%s", (int)total, total == 1.0 ? "" : "s");
	putchar('\n');
}

/* JSON string literal straight to stdout; sbuf would cap long lists. */
static void put_json_string(const char *v) {
	putchar('"');
	for (; *v; v++) {
		unsigned char c = (unsigned char)*v;
		if (c == '"' || c == '\\') printf("\\%c", c);
		else if (c < 0x20) printf("\\u%04x", c);
		else putchar(c);
	}
	putchar('"');
}

static void print_data(const char *action, const char *data) {
	double n = 0;
	char mode[32] = "";
	if (!strcmp(action, "status")) print_status(data);
	else if (!strcmp(action, "search")) print_search(data);
	else if (!strcmp(action, "queueByID") || !strcmp(action, "queueByStore")) {
		char matched[256] = "";
		int store = !strcmp(action, "queueByStore");
		if (store && json_num(data, "queued", &n)) {
			printf("queued %d track%s", (int)n, n == 1.0 ? "" : "s");
			if (json_str(data, "mode", mode, sizeof mode)) printf(" (%s)", mode);
			putchar('\n');
		} else if (json_str(data, "mode", mode, sizeof mode)) {
			printf("ok (%s)", mode);
			if (json_str(data, "matched", matched, sizeof matched)) printf("  %s", matched);
			putchar('\n');
		} else puts("ok");
		print_queue_state(data);
	} else if (!strncmp(action, "queue", 5) && json_num(data, "queued", &n)) {
		printf("queued %d track%s", (int)n, n == 1.0 ? "" : "s");
		if (json_str(data, "mode", mode, sizeof mode)) printf(" (%s)", mode);
		putchar('\n');
	} else puts("ok");
}

static void print_json_error(const char *msg) {
	sbuf b = {0};
	sb_addf(&b, "{\"error\":");
	if (sb_add_escaped(&b, msg) < 0) sb_addf(&b, "\"invalid response\"");
	sb_addf(&b, "}");
	fprintf(stderr, "%s\n", b.buf);
}

/* ---------- library database ---------- */

/*
 * Album, artist and song names are resolved here because the tweak's process
 * cannot enumerate the whole library. The database is opened read-only, only
 * by the commands that need it, and never created (SQLITE_OPEN_CREATE is not
 * passed). Every query is a prepared statement with bound parameters.
 *
 * Matching folds ASCII case only: SQL uses COLLATE NOCASE and LIKE, C uses
 * sqlite3_strnicmp. Accents are not folded ("Bjork" will not find "Björk")
 * and non-ASCII case pairs compare as raw UTF-8 bytes. Full Unicode matching
 * would need ICU, which this tool does not link.
 *
 * Column names differ between library versions, so the song title and track
 * artist locations are probed once rather than assumed: titles live in
 * item.title or item_extra.title, track artists in item_artist or person.
 */

static sqlite3 *db;

typedef struct {
	sqlite3_int64 id;
	long long count;
	char title[256];
} cand_t;

typedef struct {
	cand_t v[MAX_CAND];
	int n;
} cands_t;

typedef struct {
	int item_title, extra_title, item_artist, person_name;
} schema_t;

static schema_t sch;
static char sql_songs[MAX_SQL], sql_songs_artist[MAX_SQL];
static char sql_search[MAX_SQL], sql_try_songs[MAX_SQL];

static const char SQL_ALBUMS[] =
	"SELECT album_pid,"
	" (SELECT COUNT(*) FROM item i WHERE i.album_pid = album.album_pid"
	"  AND i.media_type = 8),"
	" album FROM album"
	" WHERE album.album = ?1 COLLATE NOCASE OR album.album LIKE ?2 ESCAPE '\\'";

static const char SQL_ARTISTS[] =
	"SELECT album_artist_pid,"
	" (SELECT COUNT(*) FROM item i WHERE i.album_artist_pid = album_artist.album_artist_pid"
	"  AND i.media_type = 8),"
	" album_artist FROM album_artist"
	" WHERE album_artist.album_artist = ?1 COLLATE NOCASE"
	"    OR album_artist.album_artist LIKE ?2 ESCAPE '\\'";

static const char SQL_TRY_ALBUMS[] =
	"SELECT album FROM album WHERE album LIKE ?1 ESCAPE '\\' LIMIT 5";
static const char SQL_TRY_ARTISTS[] =
	"SELECT album_artist FROM album_artist"
	" WHERE album_artist LIKE ?1 ESCAPE '\\' LIMIT 5";

/* Apple Music catalog IDs behind a resolved pid, in play order. item_store is
 * 1:1 with item; subscription ids cover cloud items, asset ids local ones. */
static const char SQL_STORE_ALBUM[] =
	"SELECT coalesce(nullif(s.subscription_store_item_id,0), s.asset_store_item_id)"
	" FROM item i JOIN item_store s ON s.item_pid = i.item_pid"
	" WHERE i.album_pid = ?1 AND i.media_type = 8"
	" ORDER BY i.disc_number, i.track_number";

static const char SQL_STORE_ARTIST[] =
	"SELECT coalesce(nullif(s.subscription_store_item_id,0), s.asset_store_item_id)"
	" FROM item i JOIN item_store s ON s.item_pid = i.item_pid"
	" WHERE i.album_artist_pid = ?1 AND i.media_type = 8"
	" ORDER BY i.album_pid, i.disc_number, i.track_number";

static const char SQL_STORE_SONG[] =
	"SELECT coalesce(nullif(s.subscription_store_item_id,0), s.asset_store_item_id)"
	" FROM item i JOIN item_store s ON s.item_pid = i.item_pid"
	" WHERE i.item_pid = ?1 AND i.media_type = 8";

/* Titles for the first line of the current commands. */
static const char SQL_ALBUM_NAME[] =
	"SELECT album FROM album WHERE album_pid = ?1";
static const char SQL_ARTIST_NAME[] =
	"SELECT album_artist FROM album_artist WHERE album_artist_pid = ?1";

/* Song select without the ORDER BY tail, so the artist filter can be added. */
#define SONG_SELECT \
	"SELECT i.item_pid," \
	" (SELECT COUNT(*) FROM item x WHERE x.album_pid = i.album_pid" \
	"  AND x.media_type = 8)," \
	" %s FROM item i%s" \
	" WHERE i.media_type = 8" \
	"   AND (%s = ?1 COLLATE NOCASE OR %s LIKE ?2 ESCAPE '\\')"

static _Noreturn void local_fail(const char *fmt, ...) {
	char msg[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (opt_json) print_json_error(msg);
	else fprintf(stderr, "%s\n", msg);
	exit(1);
}

static sqlite3_stmt *prepare(sqlite3 *h, const char *sql) {
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK)
		local_fail("library query failed: %s", sqlite3_errmsg(h));
	return st;
}

static sqlite3 *open_db(void) {
	const char *path = getenv("MUSICREMOTE_DB");
	if (!path || !*path) path = DB_PATH;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		char msg[PATH_MAX + 128];
		snprintf(msg, sizeof msg, "cannot open library database %s: %s", path,
		         db ? sqlite3_errmsg(db) : "out of memory");
		sqlite3_close(db);
		db = NULL;
		local_fail("%s", msg);
	}
	sqlite3_busy_timeout(db, 1000);
	/* Force the first read: a WAL database without its -shm opens fine and
	 * only fails when read, and that error would name no path. */
	if (sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", NULL, NULL, NULL) != SQLITE_OK) {
		char msg[PATH_MAX + 128];
		snprintf(msg, sizeof msg, "cannot read library database %s: %s",
		         path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		local_fail("%s", msg);
	}
	return db;
}

static void close_db(sqlite3 *h) {
	if (h) sqlite3_close(h);
	db = NULL;
}

static int has_column(sqlite3 *h, const char *table, const char *col) {
	char sql[96];
	snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
	sqlite3_stmt *st = prepare(h, sql);
	int found = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *n = sqlite3_column_text(st, 1);
		if (n && !strcmp((const char *)n, col)) { found = 1; break; }
	}
	sqlite3_finalize(st);
	return found;
}

static void build_sql(void) {
	const char *title = sch.item_title ? "i.title" : "e.title";
	const char *tj = sch.item_title ? "" :
	                 " LEFT JOIN item_extra e ON e.item_pid = i.item_pid";
	const char *artist = sch.item_artist ? "ia.item_artist" :
	                     sch.person_name ? "p.name" : "''";
	const char *aj = sch.item_artist ?
	                 " LEFT JOIN item_artist ia ON ia.item_artist_pid = i.item_artist_pid" :
	                 sch.person_name ? " LEFT JOIN person p ON p.person_pid = i.item_artist_pid" : "";

	snprintf(sql_songs, sizeof sql_songs, SONG_SELECT " ORDER BY i.item_pid",
	         title, tj, title, title);
	if (sch.item_artist || sch.person_name) {
		const char *sub = sch.item_artist ?
			"SELECT item_artist_pid FROM item_artist"
			" WHERE item_artist LIKE ?3 ESCAPE '\\'" :
			"SELECT person_pid FROM person WHERE name LIKE ?3 ESCAPE '\\'";
		snprintf(sql_songs_artist, sizeof sql_songs_artist, SONG_SELECT
		         "   AND (i.album_artist_pid IN"
		         " (SELECT album_artist_pid FROM album_artist"
		         "   WHERE album_artist LIKE ?3 ESCAPE '\\')"
		         "  OR i.item_artist_pid IN (%s))"
		         " ORDER BY i.item_pid",
		         title, tj, title, title, sub);
	} else {
		sql_songs_artist[0] = '\0';
	}

	snprintf(sql_search, sizeof sql_search,
	         "SELECT %s, COALESCE(NULLIF(%s, ''), aa.album_artist, ''),"
	         " COALESCE(a.album, '')"
	         " FROM item i%s%s"
	         " LEFT JOIN album a ON a.album_pid = i.album_pid"
	         " LEFT JOIN album_artist aa ON aa.album_artist_pid = i.album_artist_pid"
	         " WHERE i.media_type = 8"
	         "   AND (%s LIKE ?1 ESCAPE '\\' OR %s LIKE ?1 ESCAPE '\\'"
	         "        OR aa.album_artist LIKE ?1 ESCAPE '\\')"
	         " ORDER BY %s COLLATE NOCASE LIMIT ?2",
	         title, artist, tj, aj, title, artist, title);

	snprintf(sql_try_songs, sizeof sql_try_songs,
	         "SELECT %s FROM item i%s WHERE i.media_type = 8"
	         " AND %s LIKE ?1 ESCAPE '\\' LIMIT 5", title, tj, title);
}

static void ensure_schema(sqlite3 *h) {
	static int ready;
	if (ready) return;
	sch.item_title = has_column(h, "item", "title");
	sch.extra_title = has_column(h, "item_extra", "title");
	sch.item_artist = has_column(h, "item_artist", "item_artist");
	sch.person_name = has_column(h, "person", "name");
	if (!sch.item_title && !sch.extra_title)
		local_fail("library database has no song title column");
	build_sql();
	ready = 1;
}

/* ASCII-only case folding, matching COLLATE NOCASE on the SQL side. */
static int ieq(const char *a, const char *b) {
	size_t n = strlen(a);
	return n == strlen(b) && sqlite3_strnicmp(a, b, (int)n) == 0;
}

static int icontains(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	if (n == 0) return 1;
	for (; *hay; hay++)
		if (sqlite3_strnicmp(hay, needle, (int)n) == 0) return 1;
	return 0;
}

/* Builds "%q%" with LIKE metacharacters escaped; -1 when q is too long. */
static int like_pattern(char *out, size_t n, const char *q) {
	size_t j = 0;
	if (strlen(q) > MAX_QUERY || n < MAX_QUERY * 2 + 4) return -1;
	out[j++] = '%';
	for (const char *p = q; *p; p++) {
		if (*p == '\\' || *p == '%' || *p == '_') out[j++] = '\\';
		out[j++] = *p;
	}
	out[j++] = '%';
	out[j] = '\0';
	return 0;
}

static void cands_add(cands_t *c, sqlite3_int64 id, long long count, const char *title) {
	for (int i = 0; i < c->n; i++)
		if (c->v[i].id == id) return;
	if (c->n >= MAX_CAND) return;
	c->v[c->n].id = id;
	c->v[c->n].count = count;
	snprintf(c->v[c->n].title, sizeof c->v[c->n].title, "%s", title ? title : "");
	c->n++;
}

/* Reads (pid, item count, title) rows into c. */
static void cands_read(sqlite3 *h, sqlite3_stmt *st, cands_t *c) {
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(st, 2);
		/* Negative album/artist pids are normal; keep the raw int64. */
		cands_add(c, sqlite3_column_int64(st, 0),
		          (long long)sqlite3_column_int64(st, 1),
		          t ? (const char *)t : "");
	}
	if (rc != SQLITE_DONE) local_fail("library query failed: %s", sqlite3_errmsg(h));
}

/* Exact match wins; otherwise the substring match with the most items. */
static const cand_t *pick_match(const cands_t *c, const char *q, int *hits) {
	const cand_t *best = NULL;
	*hits = 0;
	for (int i = 0; i < c->n; i++) {
		if (!ieq(c->v[i].title, q)) continue;
		*hits = 1;
		if (!best || c->v[i].count > best->count) best = &c->v[i];
	}
	if (best) return best;
	for (int i = 0; i < c->n; i++) {
		if (!icontains(c->v[i].title, q)) continue;
		(*hits)++;
		if (!best || c->v[i].count > best->count) best = &c->v[i];
	}
	return best;
}

/* Notes ambiguous substring matches on stderr; stdout stays clean for --json. */
static void note_others(const cands_t *c, const cand_t *best, const char *q,
                        const char *what, int hits, int show_count) {
	if (hits < 2) return;
	fprintf(stderr, "note: %d %ss match \"%s\"; using \"%s\"", hits, what, q, best->title);
	if (show_count) fprintf(stderr, " (%lld tracks)", best->count);
	int shown = 0;
	for (int i = 0; i < c->n && shown < 3; i++) {
		if (&c->v[i] == best || !icontains(c->v[i].title, q)) continue;
		fprintf(stderr, "%s\"%s\"", shown ? ", " : "; others: ", c->v[i].title);
		shown++;
	}
	fputc('\n', stderr);
}

/* Up to 5 titles matching any word of q, for a "no match" hint. */
static void suggest(sqlite3 *h, const char *sql, const char *q, char *out, size_t outsz) {
	char words[MAX_QUERY + 1], found[SUGGEST_MAX][256];
	int nf = 0;
	snprintf(words, sizeof words, "%s", q);
	char *save = NULL;
	for (char *w = strtok_r(words, " \t", &save); w && nf < SUGGEST_MAX;
	     w = strtok_r(NULL, " \t", &save)) {
		char pat[MAX_QUERY * 2 + 4];
		if (like_pattern(pat, sizeof pat, w) < 0) continue;
		sqlite3_stmt *st = prepare(h, sql);
		sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
		int rc;
		while (nf < SUGGEST_MAX && (rc = sqlite3_step(st)) == SQLITE_ROW) {
			const unsigned char *t = sqlite3_column_text(st, 0);
			if (!t || !*t) continue;
			int dup = 0;
			for (int i = 0; i < nf; i++)
				if (ieq(found[i], (const char *)t)) dup = 1;
			if (!dup) snprintf(found[nf++], sizeof found[0], "%s", (const char *)t);
		}
		sqlite3_finalize(st);
	}
	out[0] = '\0';
	for (int i = 0; i < nf; i++) {
		size_t used = strlen(out), room = used < outsz ? outsz - used : 0;
		if (room < 2) break;
		snprintf(out + used, room, "%s\"%s\"", i ? ", " : " (closest: ", found[i]);
	}
	if (nf) {
		size_t used = strlen(out), room = used < outsz ? outsz - used : 0;
		if (room >= 2) snprintf(out + used, room, ")");
	}
}

static sqlite3_int64 resolve_named(sqlite3 *h, const char *what, const char *q,
                                   const char *sql, const char *sql_try) {
	char pat[MAX_QUERY * 2 + 4];
	if (!*q) local_fail("%s name is empty", what);
	if (like_pattern(pat, sizeof pat, q) < 0)
		local_fail("%s name is too long (max %d bytes)", what, MAX_QUERY);
	cands_t c = {0};
	sqlite3_stmt *st = prepare(h, sql);
	sqlite3_bind_text(st, 1, q, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, pat, -1, SQLITE_TRANSIENT);
	cands_read(h, st, &c);
	sqlite3_finalize(st);

	int hits = 0;
	const cand_t *best = pick_match(&c, q, &hits);
	if (!best) {
		char hint[512] = "", msg[768];
		suggest(h, sql_try, q, hint, sizeof hint);
		snprintf(msg, sizeof msg, "no %s matched \"%s\"%s", what, q, hint);
		local_fail("%s", msg);
	}
	note_others(&c, best, q, what, hits, 1);
	return best->id;
}

static sqlite3_int64 resolve_song(sqlite3 *h, const char *q, const char *artist) {
	char pat[MAX_QUERY * 2 + 4], apat[MAX_QUERY * 2 + 4];
	if (!*q) local_fail("song name is empty");
	if (like_pattern(pat, sizeof pat, q) < 0)
		local_fail("song name is too long (max %d bytes)", MAX_QUERY);
	if (artist && !sql_songs_artist[0])
		local_fail("library database has no track artist column to filter on");
	cands_t c = {0};
	sqlite3_stmt *st = prepare(h, artist ? sql_songs_artist : sql_songs);
	sqlite3_bind_text(st, 1, q, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, pat, -1, SQLITE_TRANSIENT);
	if (artist) {
		if (like_pattern(apat, sizeof apat, artist) < 0)
			local_fail("artist name is too long (max %d bytes)", MAX_QUERY);
		sqlite3_bind_text(st, 3, apat, -1, SQLITE_TRANSIENT);
	}
	cands_read(h, st, &c);
	sqlite3_finalize(st);

	int hits = 0;
	const cand_t *best = pick_match(&c, q, &hits);
	if (!best) {
		char hint[512] = "", msg[768];
		suggest(h, sql_try_songs, q, hint, sizeof hint);
		if (artist)
			snprintf(msg, sizeof msg, "no song matched \"%s\" by \"%s\"%s", q, artist, hint);
		else
			snprintf(msg, sizeof msg, "no song matched \"%s\"%s", q, hint);
		local_fail("%s", msg);
	}
	note_others(&c, best, q, "song", hits, 0);
	return best->id;
}

/* Collects up to MAX_STORE_IDS usable store IDs in play order. Errors when the
 * selection is bigger, with the real count, so the caller can narrow it. */
static int store_ids(sqlite3 *h, const char *sql, sqlite3_int64 pid,
                     long long *ids, const char *what) {
	sqlite3_stmt *st = prepare(h, sql);
	sqlite3_bind_int64(st, 1, pid);
	int n = 0, rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		if (sqlite3_column_type(st, 0) == SQLITE_NULL) continue;
		long long id = (long long)sqlite3_column_int64(st, 0);
		if (id == 0) continue;
		if (n < MAX_STORE_IDS) ids[n] = id;
		n++;
	}
	if (rc != SQLITE_DONE) {
		sqlite3_finalize(st);
		local_fail("library query failed: %s", sqlite3_errmsg(h));
	}
	sqlite3_finalize(st);
	if (n > MAX_STORE_IDS)
		local_fail("that %s has %d tracks, over the %d limit", what, n, MAX_STORE_IDS);
	return n;
}

/* {"ids":["1099438666",...],"start":"1099438683","mode":"replace"}; the start
 * key is omitted when start is NULL. Caller frees. */
static char *store_args(const long long *ids, int n, const char *mode,
                        const long long *start) {
	dbuf d = {0};
	dbuf_add(&d, "{\"ids\":[");
	for (int i = 0; i < n; i++) {
		char num[32];
		snprintf(num, sizeof num, "%s%lld\"", i ? ",\"" : "\"", ids[i]);
		dbuf_add(&d, num);
	}
	dbuf_add(&d, "],");
	if (start) {
		char num[32];
		snprintf(num, sizeof num, "\"start\":\"%lld\",", *start);
		dbuf_add(&d, num);
	}
	dbuf_add(&d, "\"mode\":\"");
	dbuf_add(&d, mode);
	dbuf_add(&d, "\"}");
	return d.buf;
}

/* Persistent IDs go on the wire as unsigned decimals: SQLite may hand back a
 * negative int64 and the tweak parses the string with strtoull. */
static void pid_string(sqlite3_int64 pid, char *out, size_t n) {
	snprintf(out, n, "%llu", (unsigned long long)(uint64_t)pid);
}

/* The status payload carries 64-bit IDs as decimal strings, normally the
 * unsigned form of a pid. strtoull folds either sign back to the same bits. */
static sqlite3_int64 parse_pid(const char *s) {
	return (sqlite3_int64)strtoull(s, NULL, 10);
}

/* Position of id in an ordered store list, or -1. */
static int index_of_id(const long long *ids, int n, long long id) {
	for (int i = 0; i < n; i++)
		if (ids[i] == id) return i;
	return -1;
}

/* One-row text lookup; out is empty when the pid has no row. */
static void lookup_name(sqlite3 *h, const char *sql, sqlite3_int64 pid,
                        char *out, size_t n) {
	out[0] = '\0';
	sqlite3_stmt *st = prepare(h, sql);
	sqlite3_bind_int64(st, 1, pid);
	int rc = sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		const unsigned char *t = sqlite3_column_text(st, 0);
		if (t) snprintf(out, n, "%s", (const char *)t);
	} else if (rc != SQLITE_DONE) {
		sqlite3_finalize(st);
		local_fail("library query failed: %s", sqlite3_errmsg(h));
	}
	sqlite3_finalize(st);
}

static long long db_count(sqlite3 *h, const char *sql) {
	sqlite3_stmt *st = prepare(h, sql);
	int rc = sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		local_fail("library query failed: %s", sqlite3_errmsg(h));
	}
	long long n = (long long)sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	return n;
}

static int cmd_library(int as_json) {
	sqlite3 *h = open_db();
	long long songs = db_count(h, "SELECT COUNT(*) FROM item WHERE media_type = 8");
	long long albums = db_count(h, "SELECT COUNT(*) FROM album");
	long long artists = db_count(h, "SELECT COUNT(*) FROM album_artist");
	close_db(h);
	if (as_json)
		printf("{\"songs\":%lld,\"albums\":%lld,\"artists\":%lld}\n", songs, albums, artists);
	else
		printf("%lld songs, %lld albums, %lld artists\n", songs, albums, artists);
	return 0;
}

typedef struct { char title[256], artist[256], album[256]; } row_t;

static int cmd_search(const char *q, long limit, int as_json) {
	char pat[MAX_QUERY * 2 + 4];
	if (!*q) local_fail("search query is empty");
	if (like_pattern(pat, sizeof pat, q) < 0)
		local_fail("search query is too long (max %d bytes)", MAX_QUERY);
	sqlite3 *h = open_db();
	ensure_schema(h);

	static row_t rows[SEARCH_MAX];
	int n = 0, rc;
	sqlite3_stmt *st = prepare(h, sql_search);
	sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, (int)limit);
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		if (n == SEARCH_MAX) break;
		const unsigned char *t = sqlite3_column_text(st, 0);
		const unsigned char *a = sqlite3_column_text(st, 1);
		const unsigned char *b = sqlite3_column_text(st, 2);
		snprintf(rows[n].title, sizeof rows[n].title, "%s", t ? (const char *)t : "");
		snprintf(rows[n].artist, sizeof rows[n].artist, "%s", a ? (const char *)a : "");
		snprintf(rows[n].album, sizeof rows[n].album, "%s", b ? (const char *)b : "");
		n++;
	}
	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		sqlite3_finalize(st);
		local_fail("library query failed: %s", sqlite3_errmsg(h));
	}
	sqlite3_finalize(st);
	close_db(h);

	if (as_json) {
		printf("{\"count\":%d,\"items\":[", n);
		for (int i = 0; i < n; i++) {
			printf("%s{\"title\":", i ? "," : "");
			put_json_string(rows[i].title);
			fputs(",\"artist\":", stdout);
			put_json_string(rows[i].artist);
			fputs(",\"album\":", stdout);
			put_json_string(rows[i].album);
			putchar('}');
		}
		puts("]}");
		return 0;
	}
	printf("%d result%s\n", n, n == 1 ? "" : "s");
	for (int i = 0; i < n; i++) {
		printf("%d. %s", i + 1, rows[i].title[0] ? rows[i].title : "(untitled)");
		if (rows[i].artist[0]) printf(" - %s", rows[i].artist);
		if (rows[i].album[0]) printf("  [%s]", rows[i].album);
		putchar('\n');
	}
	return 0;
}

/* ---------- commands ---------- */

static void usage(void) {
	fputs("usage: musicremote [--json] <command> [args]\n"
	      "  status\n"
	      "  play | pause | toggle | next | prev\n"
	      "  album|artist <query> [--replace|--append|--next]\n"
	      "  song <query> [--artist <name>] [--replace|--append|--next]\n"
	      "  queue album [--next | --rest | --append | --from-start]\n"
	      "  queue artist [--next | --append]\n"
	      "  search <query> [--limit N]\n"
	      "  library\n"
	      "  state\n"
	      "  count songs|albums|artists\n"
	      "  volume <0.0-1.0>\n"
	      "  raw '<json object containing \"action\">'\n"
	      "--json prints the raw response data\n"
	      "MUSICREMOTE_DIR and MUSICREMOTE_DB override the paths (testing)\n", stderr);
}

static int usage_error(const char *msg) { fprintf(stderr, "%s\n", msg); usage(); return 64; }

static const char *mode_flag(const char *a) {
	if (!strcmp(a, "--replace")) return "replace";
	if (!strcmp(a, "--append")) return "append";
	if (!strcmp(a, "--next")) return "next";
	return NULL;
}

/* Sends one request over the shared transport and stores the response data in
 * *out (never NULL on success). Transport errors are printed here, honouring
 * --json, and returned as the exit code. Nothing is printed on success. */
static int exec_request(const request_t *req, int as_json, char **out) {
	*out = NULL;
	if (mkdir_p(dir_path) != 0) {
		fprintf(stderr, "cannot create %s: %s\n", dir_path, strerror(errno));
		return 1;
	}
	if (take_lock() < 0) {
		fputs("another musicremote request is in progress\n", stderr);
		return 2;
	}

	/* The body grows with the id list: a 600-track store queue is far past
	 * the old 8 KB stack buffer. Caller caps the list at MAX_STORE_IDS. */
	char id[17];
	snprintf(id, sizeof id, "%08X%08X", (unsigned)arc4random(), (unsigned)arc4random());
	size_t blen = strlen(req->action) + strlen(req->args) + 64;
	char *body = malloc(blen);
	if (!body) oom();
	int n = snprintf(body, blen, "{\"id\":\"%s\",\"action\":\"%s\",\"args\":%s}",
	                 id, req->action, req->args);
	if (n < 0 || (size_t)n >= blen) {
		fputs("request too large\n", stderr);
		free(body);
		return 1;
	}
	if (write_atomic(req_path, tmp_path, body, (size_t)n) != 0) {
		fprintf(stderr, "cannot write %s: %s\n", req_path, strerror(errno));
		free(body);
		return 1;
	}
	free(body);

	char *data = NULL, *error = NULL;
	int r = RESP_NONE;
	// Darwin notifications can be coalesced or missed, and the tweak ignores a
	// request whose file it already consumed, so re-posting is safe.
	for (int attempt = 0; attempt < MAX_ATTEMPTS && r == RESP_NONE; attempt++) {
		CFStringRef note = CFStringCreateWithCString(NULL, NOTIFY_REQ, kCFStringEncodingUTF8);
		if (note) {
			CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
			                                    note, NULL, NULL, true);
			CFRelease(note);
		}
		r = poll_response(id, &data, &error, ATTEMPT_MS);
	}
	unlink(req_path);

	if (r == RESP_NONE) {
		fputs("no answer from SpringBoard, the tweak is not loaded\n", stderr);
		return 2;
	}
	if (r == RESP_ERR) {
		if (as_json) print_json_error(error);
		else fprintf(stderr, "%s\n", error);
		free(error);
		return 1;
	}
	if (!data) {
		data = strdup("{}");
		if (!data) oom();
	}
	*out = data;
	return 0;
}

static int run_request(const request_t *req, int as_json) {
	char *data = NULL;
	int rc = exec_request(req, as_json, &data);
	if (rc != 0) return rc;
	if (as_json) printf("%s\n", data);
	else print_data(req->action, data);
	free(data);
	return 0;
}

static int cmd_raw(int ac, char **av, int as_json) {
	if (ac < 2) return usage_error("raw needs a JSON args object");
	char action[64];
	if (!json_str(av[1], "action", action, sizeof action) || !action[0])
		return usage_error("raw object must contain an \"action\" string");
	const char *p = skip_ws(av[1]), *e = skip_value(p);
	if (*p != '{' || !e || *skip_ws(e))
		return usage_error("raw argument must be a single JSON object");
	request_t req = { action, av[1] };
	return run_request(&req, as_json);
}

/* ---------- current ---------- */

typedef struct {
	char state[32], album[256], artist[256];
	char album_id[24], album_artist_id[24], track_id[24];
} now_playing_t;

static void read_now_playing(const char *data, now_playing_t *np) {
	memset(np, 0, sizeof *np);
	json_str(data, "state", np->state, sizeof np->state);
	json_str(data, "album", np->album, sizeof np->album);
	json_str(data, "artist", np->artist, sizeof np->artist);
	json_str(data, "albumID", np->album_id, sizeof np->album_id);
	json_str(data, "albumArtistID", np->album_artist_id, sizeof np->album_artist_id);
	json_str(data, "trackID", np->track_id, sizeof np->track_id);
}

/* Store ID of the item that is playing right now, or 0 when unknown. Used to
 * keep the playing track out of an album/artist queue so it is not heard twice. */
static long long playing_store_id(void) {
	request_t sreq = { "status", "{}" };
	char *status = NULL;
	if (exec_request(&sreq, 0, &status) != 0) {
		free(status);
		return 0;
	}
	now_playing_t np;
	read_now_playing(status, &np);
	free(status);
	if (!np.track_id[0]) return 0;
	sqlite3 *h = open_db();
	long long one[1] = { 0 };
	int n = store_ids(h, SQL_STORE_SONG, parse_pid(np.track_id), one, "song");
	close_db(h);
	return n > 0 ? one[0] : 0;
}

/* One word for the add-on's condition, and a number for the count conditions. */
static int cmd_state(int as_json) {
	request_t req = { "status", "{}" };
	char *data = NULL;
	int rc = exec_request(&req, as_json, &data);
	if (rc != 0) return rc;
	now_playing_t np;
	read_now_playing(data, &np);
	free(data);
	if (as_json) printf("{\"state\":\"%s\"}\n", np.state[0] ? np.state : "unknown");
	else printf("%s\n", np.state[0] ? np.state : "unknown");
	return 0;
}

static int cmd_count(const char *what, int as_json) {
	const char *sql = !strcmp(what, "songs")
		? "SELECT count(*) FROM item WHERE media_type = 8"
		: !strcmp(what, "albums") ? "SELECT count(*) FROM album"
		: !strcmp(what, "artists") ? "SELECT count(*) FROM album_artist" : NULL;
	if (sql == NULL) return usage_error("count needs songs, albums or artists");
	sqlite3 *h = open_db();
	ensure_schema(h);
	sqlite3_stmt *st = prepare(h, sql);
	long long n = 0;
	if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
	sqlite3_finalize(st);
	close_db(h);
	if (as_json) printf("{\"count\":%lld}\n", n);
	else printf("%lld\n", n);
	return 0;
}

enum { CUR_FROM_START = 1, CUR_REST = 2, CUR_APPEND = 4, CUR_NEXT = 8 };

/* queue album|artist turns a status reply into a queueByStore request. The
 * track list comes from the same database resolution as the name commands, so
 * the queue matches the library's own album/artist and play order. `current`
 * is kept as an alias. */
static int cmd_current(int ac, char **av, int as_json) {
	if (ac < 2 || (strcmp(av[1], "album") && strcmp(av[1], "artist")))
		return usage_error("queue needs album or artist");
	const char *what = av[1];
	int is_album = !strcmp(what, "album");
	int flags = 0;
	for (int i = 2; i < ac; i++) {
		int bit = !strcmp(av[i], "--from-start") ? CUR_FROM_START
		        : !strcmp(av[i], "--rest") ? CUR_REST
		        : !strcmp(av[i], "--append") ? CUR_APPEND
		        : !strcmp(av[i], "--next") ? CUR_NEXT : 0;
		if (!bit)
			return usage_error(is_album
			                   ? "queue album takes --next, --rest, --append or --from-start"
			                   : "queue artist takes only --append or --next");
		if (!is_album && (bit == CUR_FROM_START || bit == CUR_REST))
			return usage_error("queue artist takes only --append or --next");
		if (flags & bit) return usage_error("duplicate flag");
		flags |= bit;
	}
	if (flags && (flags & (flags - 1)))
		return usage_error("pick one of --from-start, --rest, --append or --next");
	/* queue album|artist plays the rest of it next, which is what "queue"
	 * means. Only an explicit flag changes that. */
	if (flags == 0) flags = CUR_NEXT;

	request_t sreq = { "status", "{}" };
	char *status = NULL;
	int rc = exec_request(&sreq, as_json, &status);
	if (rc != 0) return rc;
	now_playing_t np;
	read_now_playing(status, &np);
	free(status);
	/* No IDs, or a blank state with no album, means nothing is loaded. */
	int have_id = is_album ? np.album_id[0] : np.album_artist_id[0];
	if (!have_id || (!np.state[0] && !np.album_id[0]))
		local_fail("nothing is playing");

	sqlite3_int64 pid = parse_pid(is_album ? np.album_id : np.album_artist_id);
	sqlite3 *h = open_db();
	long long ids[MAX_STORE_IDS];
	int n = store_ids(h, is_album ? SQL_STORE_ALBUM : SQL_STORE_ARTIST, pid, ids, what);
	if (n == 0) {
		close_db(h);
		local_fail("no stream ids for this %s", what);
	}
	char name[256] = "";
	lookup_name(h, is_album ? SQL_ALBUM_NAME : SQL_ARTIST_NAME, pid, name, sizeof name);
	if (!name[0]) snprintf(name, sizeof name, "%s", is_album ? np.album : np.artist);
	if (!name[0]) snprintf(name, sizeof name, "(unknown)");

	/* --rest and the default both need the playing track's store ID, found by
	 * position in the ordered list. That position is exactly what the player
	 * will play, so disc/track numbers are not used for it. */
	int track_index = -1;
	if (np.track_id[0]) {
		long long track_store[1];
		if (store_ids(h, SQL_STORE_SONG, parse_pid(np.track_id), track_store, "song") > 0)
			track_index = index_of_id(ids, n, track_store[0]);
	}
	close_db(h);

	const long long *send_ids = ids;
	int send_n = n;
	const char *mode = "replace";
	long long start_id = 0;
	const long long *start = NULL;

	if (flags & CUR_REST) {
		if (track_index < 0 || track_index + 1 >= n)
			local_fail("nothing after this track");
		send_ids = ids + track_index + 1;
		send_n = n - track_index - 1;
		mode = "append";
	} else if (flags & CUR_NEXT) {
		/* Insert after the now playing item, so it starts when this song ends. */
		if (track_index < 0 || track_index + 1 >= n)
			local_fail("nothing after this track");
		send_ids = ids + track_index + 1;
		send_n = n - track_index - 1;
		mode = "next";
	} else if (flags & CUR_APPEND) {
		mode = "append";
		/* Keep the playing track out of the appended copy. */
		if (track_index >= 0) {
			int kept = 0;
			for (int i = 0; i < n; i++)
				if (i != track_index) ids[kept++] = ids[i];
			n = kept;
			send_n = n;
			if (send_n == 0) local_fail("that track is the only one and it is already playing");
		}
	} else if (!(flags & CUR_FROM_START)) {
		if (track_index >= 0) {
			start_id = ids[track_index];
			start = &start_id;
		}
	}

	char *qargs = store_args(send_ids, send_n, mode, start);
	request_t qreq = { "queueByStore", qargs };
	char *qdata = NULL;
	rc = exec_request(&qreq, as_json, &qdata);
	free(qargs);
	if (rc != 0) return rc;
	if (as_json) {
		printf("%s\n", qdata);
	} else {
		printf("%s: %s\n", what, name);
		print_data("queueByStore", qdata);
	}
	free(qdata);
	return 0;
}

int main(int argc, char **argv) {
	char **av = malloc(sizeof(char *) * (size_t)(argc + 1));
	if (!av) oom();
	int as_json = 0, ac = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) as_json = 1;
		else av[ac++] = argv[i];
	}
	av[ac] = NULL;
	opt_json = as_json;
	init_paths();

	if (ac == 0) { usage(); return 64; }
	if (!strcmp(av[0], "help") || !strcmp(av[0], "-h") || !strcmp(av[0], "--help")) {
		usage();
		return 0;
	}
	if (!strcmp(av[0], "raw")) return cmd_raw(ac, av, as_json);
	if (!strcmp(av[0], "state")) return cmd_state(as_json);
	if (!strcmp(av[0], "count")) return cmd_count(ac > 1 ? av[1] : "", as_json);
	if (!strcmp(av[0], "current") || !strcmp(av[0], "queue"))
		return cmd_current(ac, av, as_json);
	if (!strcmp(av[0], "library")) {
		if (ac != 1) return usage_error("this command takes no arguments");
		return cmd_library(as_json);
	}

	const char *a = av[0];
	request_t req = { NULL, NULL };
	char *owned_args = NULL;
	sbuf args = {0};
	sb_addf(&args, "{");

	if (!strcmp(a, "status") || !strcmp(a, "play") || !strcmp(a, "pause") ||
	    !strcmp(a, "toggle") || !strcmp(a, "next") || !strcmp(a, "prev")) {
		if (ac != 1) return usage_error("this command takes no arguments");
		req.action = a;
	} else if (!strcmp(a, "album") || !strcmp(a, "artist") || !strcmp(a, "song")) {
		/* Words without quotes join into one query, and flags may appear
		 * before or after them. For song, words after --artist are the
		 * artist name. rc flattens argv, so this is what makes
		 * "rc music album Kind of Blue" work. */
		char query[4096] = "", artist[4096] = "";
		int in_artist = 0;
		const char *mode = "replace";
		for (int i = 1; i < ac; i++) {
			const char *m = mode_flag(av[i]);
			if (m) { mode = m; continue; }
			if (!strcmp(a, "song") && !strcmp(av[i], "--artist")) {
				in_artist = 1;
				continue;
			}
			if (av[i][0] == '-' && av[i][1] == '-')
				return usage_error(!strcmp(a, "song")
				                   ? "expected --artist, --replace, --append or --next"
				                   : "expected --replace, --append or --next");
			char *dst = in_artist ? artist : query;
			size_t cap = in_artist ? sizeof artist : sizeof query;
			if (query_add(dst, cap, av[i]) != 0)
				return usage_error("query is too long");
		}
		if (!query[0]) return usage_error("this command needs a query");
		if (in_artist && !artist[0]) return usage_error("--artist needs a name");

		/* Resolve the name, read its store IDs in play order, and queue
		 * those: queueByStore needs no library access in the tweak. Fall
		 * back to the old persistent-ID path when nothing has a store ID. */
		sqlite3 *h = open_db();
		ensure_schema(h);
		sqlite3_int64 pid;
		const char *store_sql = SQL_STORE_ALBUM;
		if (!strcmp(a, "album"))
			pid = resolve_named(h, "album", query, SQL_ALBUMS, SQL_TRY_ALBUMS);
		else if (!strcmp(a, "artist")) {
			pid = resolve_named(h, "artist", query, SQL_ARTISTS, SQL_TRY_ARTISTS);
			store_sql = SQL_STORE_ARTIST;
		} else {
			pid = resolve_song(h, query, artist[0] ? artist : NULL);
			store_sql = SQL_STORE_SONG;
		}
		long long ids[MAX_STORE_IDS];
		int n = store_ids(h, store_sql, pid, ids, a);
		close_db(h);

		/* next and append must not re-queue the track already playing. */
		if (n > 0 && (!strcmp(mode, "next") || !strcmp(mode, "append"))) {
			long long playing = playing_store_id();
			if (playing != 0) {
				int kept = 0;
				for (int i = 0; i < n; i++)
					if (ids[i] != playing) ids[kept++] = ids[i];
				if (kept == 0) local_fail("that track is the only one and it is already playing");
				n = kept;
			}
		}

		if (n > 0) {
			req.action = "queueByStore";
			owned_args = store_args(ids, n, mode, NULL);
			req.args = owned_args;
		} else {
			fprintf(stderr, "note: no stream ids for this %s, using library path\n", a);
			char pidbuf[24];
			pid_string(pid, pidbuf, sizeof pidbuf);
			req.action = "queueByID";
			if (arg_str(&args, "property", a) != 0 ||
			    arg_str(&args, "id", pidbuf) != 0 ||
			    arg_str(&args, "mode", mode) != 0)
				return usage_error("argument contains control characters or is too long");
		}
	} else if (!strcmp(a, "search")) {
		char query[4096] = "";
		long limit = DEFAULT_LIMIT;
		for (int i = 1; i < ac; i++) {
			if (!strcmp(av[i], "--limit")) {
				if (++i >= ac) return usage_error("--limit needs a number");
				char *end = NULL;
				limit = strtol(av[i], &end, 10);
				if (end == av[i] || *end || limit <= 0)
					return usage_error("--limit needs a positive number");
				if (limit > SEARCH_MAX) limit = SEARCH_MAX;
				continue;
			}
			if (av[i][0] == '-' && av[i][1] == '-')
				return usage_error("expected --limit N");
			if (query_add(query, sizeof query, av[i]) != 0)
				return usage_error("query is too long");
		}
		if (!query[0]) return usage_error("search needs a query");
		return cmd_search(query, limit, as_json);
	} else if (!strcmp(a, "volume")) {
		if (ac != 2) return usage_error("volume needs a value between 0.0 and 1.0");
		char *end = NULL;
		double v = strtod(av[1], &end);
		if (end == av[1] || *end != '\0' || !(v >= 0.0 && v <= 1.0))
			return usage_error("volume needs a value between 0.0 and 1.0");
		req.action = "volume";
		arg_num(&args, "value", v);
	} else {
		usage();
		return 64;
	}

	sb_addf(&args, "}");
	if (!owned_args) req.args = args.buf;
	int rc = run_request(&req, as_json);
	free(owned_args);
	return rc;
}
