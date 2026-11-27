/*
 * musicremote sends one command to Music.app through the MusicRemote tweak:
 * write req.json {"id","action","args"} (tmp+fsync+rename), post the Darwin
 * notification com.saihgupr.musicremote.req, poll res.json every 50 ms for 3 s
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define DIR_PATH        "/var/mobile/Media/.musicremote"
#define DB_PATH         "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb"
#define NOTIFY_REQ      "com.saihgupr.musicremote.req"
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

static void dbuf_addc(dbuf *d, char c) {
	dbuf_reserve(d, 1);
	d->buf[d->len++] = c;
	d->buf[d->len] = '\0';
}

static void dbuf_addf(dbuf *d, const char *fmt, ...) {
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) { va_end(ap2); return; }
	dbuf_reserve(d, (size_t)n);
	vsnprintf(d->buf + d->len, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	d->len += (size_t)n;
}

static void dbuf_add_escaped(dbuf *d, const char *v) {
	dbuf_addc(d, '"');
	for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') { dbuf_addc(d, '\\'); dbuf_addc(d, (char)c); }
		else if (c == '\b') dbuf_add(d, "\\b");
		else if (c == '\f') dbuf_add(d, "\\f");
		else if (c == '\n') dbuf_add(d, "\\n");
		else if (c == '\r') dbuf_add(d, "\\r");
		else if (c == '\t') dbuf_add(d, "\\t");
		else if (c < 0x20) dbuf_addf(d, "\\u%04x", c);
		else dbuf_addc(d, (char)c);
	}
	dbuf_addc(d, '"');
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

static int json_bool(const char *obj, const char *key, int *out) {
	const char *v = json_value(obj, key);
	if (!v) return 0;
	if (!strncmp(v, "true", 4)) { *out = 1; return 1; }
	if (!strncmp(v, "false", 5)) { *out = 0; return 1; }
	double d = 0;
	if (json_num(obj, key, &d)) { *out = d != 0.0; return 1; }
	return 0;
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

/* ---------- catalogue album lookup ---------- */

/* The library only knows the tracks that were added to it, which is often a
 * few songs out of a full album. iTunes' public lookup API returns the whole
 * running order for a store ID, so an album can be queued end to end even when
 * most of it was never added. No account or token needed. */
#define CURL_PATH "/var/jb/usr/bin/curl"
#define CATALOG_MAX 500
#define CATALOG_TIMEOUT "3"

typedef struct { long long id; int disc, track; } catalog_track_t;

static char *read_text_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long len = ftell(f);
	if (len < 0 || len > 8 * 1024 * 1024) { fclose(f); return NULL; }
	rewind(f);
	char *buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

/* curl with a hard timeout; false on any failure. Spawned directly because a
 * rootless device has no /bin/sh for system(). */
static int curl_get(const char *url, const char *out_path) {
	char *argv[] = { (char *)CURL_PATH, (char *)"-fsS",
	                 (char *)"--max-time", (char *)CATALOG_TIMEOUT,
	                 (char *)"-o", (char *)out_path, (char *)url, NULL };
	pid_t pid;
	int status = 0;
	if (posix_spawn(&pid, CURL_PATH, NULL, NULL, argv, environ) != 0) return 0;
	if (waitpid(pid, &status, 0) < 0) return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static long long catalog_collection_id(const char *json) {
	const char *results = json_value(json, "results");
	if (!results || *results != '[') return 0;
	const char *first = skip_ws(results + 1);
	if (*first != '{') return 0;
	double value = 0;
	if (!json_num(first, "collectionId", &value) || value <= 0) return 0;
	return (long long)value;
}

static int catalog_parse_tracks(const char *json, catalog_track_t *out, int max) {
	const char *results = json_value(json, "results");
	if (!results || *results != '[') return 0;
	int n = 0;
	const char *p = results + 1;
	for (;;) {
		p = skip_ws(p);
		if (*p == ',') { p++; continue; }
		if (*p != '{') break;
		const char *end = skip_value(p);
		if (!end) break;
		char wrapper[24] = "";
		double id = 0, disc = 1, track = 0;
		if (json_str(p, "wrapperType", wrapper, sizeof wrapper) &&
		    !strcmp(wrapper, "track") &&
		    json_num(p, "trackId", &id) && id > 0 &&
		    json_num(p, "trackNumber", &track) && track > 0) {
			if (!json_num(p, "discNumber", &disc) || disc < 1) disc = 1;
			if (n < max) {
				out[n].id = (long long)id;
				out[n].disc = (int)disc;
				out[n].track = (int)track;
				n++;
			}
		}
		p = end;
	}
	return n;
}

/* Album tracks in play order for the album that contains anchor, or 0 when the
 * lookup fails or the store does not know it. */
static int catalog_album_ids(long long anchor, long long *ids, int max) {
	if (anchor == 0 || max <= 0) return 0;
	char url[256], path[PATH_MAX];
	snprintf(path, sizeof path, "%s/catalog.json", dir_path);

	snprintf(url, sizeof url,
	         "https://itunes.apple.com/lookup?id=%lld&entity=song&limit=5", anchor);
	unlink(path);
	if (!curl_get(url, path)) { unlink(path); return 0; }
	char *body = read_text_file(path);
	unlink(path);
	if (!body) return 0;
	long long collection = catalog_collection_id(body);
	free(body);
	if (collection == 0) return 0;

	snprintf(url, sizeof url,
	         "https://itunes.apple.com/lookup?id=%lld&entity=song&limit=200", collection);
	unlink(path);
	if (!curl_get(url, path)) { unlink(path); return 0; }
	body = read_text_file(path);
	unlink(path);
	if (!body) return 0;
	catalog_track_t tracks[CATALOG_MAX];
	int n = catalog_parse_tracks(body, tracks, CATALOG_MAX);
	free(body);
	if (n <= 0) return 0;

	/* iTunes returns album order already, but discs make that an assumption. */
	for (int i = 1; i < n; i++) {
		catalog_track_t key = tracks[i];
		int j = i - 1;
		while (j >= 0 && (tracks[j].disc > key.disc ||
		                  (tracks[j].disc == key.disc && tracks[j].track > key.track))) {
			tracks[j + 1] = tracks[j];
			j--;
		}
		tracks[j + 1] = key;
	}
	if (n > max) n = max;
	for (int i = 0; i < n; i++) ids[i] = tracks[i].id;
	return n;
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
	      "  transfer [target] [--to <target>] [--from <source>] [--overlap <sec>] [--play|--pause]\n"
	      "  pull [source] [--from <source>] [--overlap <sec>] [--play|--pause]\n"
	      "  snapshot\n"
	      "  receive [--b64 <payload> | --data <json>] [--play|--pause]\n"
	      "  search <query> [--limit N]\n"
	      "  library\n"
	      "  state\n"
	      "  count songs|albums|artists\n"
	      "  love [on|off|toggle|status]\n"
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
	char state[32], title[256], album[256], artist[256];
	char album_id[24], album_artist_id[24], track_id[24], store_id[24];
} now_playing_t;

static void read_now_playing(const char *data, now_playing_t *np) {
	memset(np, 0, sizeof *np);
	json_str(data, "state", np->state, sizeof np->state);
	json_str(data, "title", np->title, sizeof np->title);
	json_str(data, "album", np->album, sizeof np->album);
	json_str(data, "artist", np->artist, sizeof np->artist);
	json_str(data, "albumID", np->album_id, sizeof np->album_id);
	json_str(data, "albumArtistID", np->album_artist_id, sizeof np->album_artist_id);
	json_str(data, "trackID", np->track_id, sizeof np->track_id);
	json_str(data, "storeID", np->store_id, sizeof np->store_id);
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
	/* A catalogue-only track reports no persistent ID, but it does report the
	 * store ID, which is all the queue needs. */
	if (np.store_id[0]) return parse_pid(np.store_id);
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

/* liked_state in item_stats: 2 is loved, 0 is not. The column name is
 * Apple's; the command and the output say love. The track id the player
 * reports is the same item_pid the database uses. */
static int db_loved_state(sqlite3 *h, sqlite3_int64 pid) {
	sqlite3_stmt *st = prepare(h, "SELECT liked_state FROM item_stats WHERE item_pid = ?");
	sqlite3_bind_int64(st, 1, pid);
	int loved = -1;
	if (sqlite3_step(st) == SQLITE_ROW) loved = sqlite3_column_int(st, 0) != 0;
	sqlite3_finalize(st);
	return loved;
}

static int cmd_love(int ac, char **av, int as_json) {
	const char *what = ac > 1 ? av[1] : "on";
	if (strcmp(what, "on") && strcmp(what, "off") && strcmp(what, "toggle") &&
	    strcmp(what, "status"))
		return usage_error("love takes on, off, toggle or status");

	request_t sreq = { "status", "{}" };
	char *status = NULL;
	int rc = exec_request(&sreq, 0, &status);
	if (rc != 0) return rc;
	now_playing_t np;
	read_now_playing(status, &np);
	free(status);
	if (!np.track_id[0]) local_fail("nothing is playing");
	sqlite3_int64 pid = parse_pid(np.track_id);

	if (!strcmp(what, "status")) {
		sqlite3 *h = open_db();
		int loved = db_loved_state(h, pid);
		close_db(h);
		if (loved < 0) local_fail("this track is not in the library");
		if (as_json) printf("{\"loved\":%s}\n", loved ? "true" : "false");
		else printf("%s\n", loved ? "loved" : "not loved");
		return 0;
	}

	if (!strcmp(what, "toggle")) {
		sqlite3 *h = open_db();
		int loved = db_loved_state(h, pid);
		close_db(h);
		if (loved < 0) local_fail("this track is not in the library");
		what = loved ? "off" : "on";
	}
	int on = !strcmp(what, "on");
	request_t req = { "love", on ? "{\"on\":true}" : "{\"on\":false}" };
	char *data = NULL;
	rc = exec_request(&req, as_json, &data);
	if (rc != 0) return rc;
	free(data);

	/* Read back from the library so the answer is what was stored, not what
	 * was asked for. The player's own cached property does not refresh. */
	sqlite3 *h = open_db();
	int loved = db_loved_state(h, pid);
	close_db(h);
	if (loved < 0) local_fail("this track is not in the library");
	if (as_json) {
		printf("{\"loved\":%s}\n", loved ? "true" : "false");
		return 0;
	}
	printf("%s  %s", loved ? "loved" : "not loved", np.title);
	if (np.artist[0]) printf(" - %s", np.artist);
	putchar('\n');
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
	/* A catalogue-only track has no library album ID, but it carries a store
	 * ID, which is enough to find the album. */
	int have_anchor = is_album ? (np.album_id[0] != 0 || np.store_id[0] != 0)
	                           : np.album_artist_id[0] != 0;
	if (!have_anchor || (!np.state[0] && !np.album_id[0] && !np.store_id[0]))
		local_fail("nothing is playing");

	sqlite3_int64 pid = is_album
		? (np.album_id[0] ? parse_pid(np.album_id) : 0)
		: parse_pid(np.album_artist_id);
	sqlite3 *h = open_db();
	long long ids[MAX_STORE_IDS];
	int n = pid != 0 ? store_ids(h, is_album ? SQL_STORE_ALBUM : SQL_STORE_ARTIST, pid, ids, what) : 0;
	char name[256] = "";
	if (pid != 0)
		lookup_name(h, is_album ? SQL_ALBUM_NAME : SQL_ARTIST_NAME, pid, name, sizeof name);
	if (!name[0]) snprintf(name, sizeof name, "%s", is_album ? np.album : np.artist);
	if (!name[0]) snprintf(name, sizeof name, "(unknown)");
	long long playing_store = np.store_id[0] ? parse_pid(np.store_id) : 0;
	if (playing_store == 0 && np.track_id[0]) {
		long long one[1];
		if (store_ids(h, SQL_STORE_SONG, parse_pid(np.track_id), one, "song") > 0)
			playing_store = one[0];
	}
	close_db(h);

	/* queue album asks the catalogue for the album's full running order, so a
	 * partly added album still plays end to end. The library list stays as the
	 * fallback when the lookup fails or the store has a different edition. */
	if (is_album && playing_store != 0) {
		long long full[MAX_STORE_IDS];
		int full_n = catalog_album_ids(playing_store, full, MAX_STORE_IDS);
		if (full_n > 0 && (n == 0 || index_of_id(full, full_n, playing_store) >= 0)) {
			memcpy(ids, full, sizeof(long long) * (size_t)full_n);
			n = full_n;
		}
	}
	if (n == 0) local_fail("no stream ids for this %s", what);

	/* --rest and the default both need the playing track's store ID, found by
	 * position in the ordered list. That position is exactly what the player
	 * will play, so disc/track numbers are not used for it. */
	int track_index = playing_store != 0 ? index_of_id(ids, n, playing_store) : -1;

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
		/* The whole album, in album order, with the playing song left out so
		 * it is not heard twice. It starts when this song ends. --rest is the
		 * flag for "only the tracks after this one". */
		mode = "next";
		if (track_index >= 0) {
			int kept = 0;
			for (int i = 0; i < n; i++)
				if (i != track_index) ids[kept++] = ids[i];
			n = kept;
			send_n = n;
			if (send_n == 0)
				local_fail("that track is the only one and it is already playing");
		}
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

/* ---------- handoff / transfer ---------- */

#define RC_PLIST_PATH   "/var/mobile/Documents/rc_triggers.plist"
#define ADDON_DOMAIN    "com.saihgupr.musicremote.addon"
#define SSH_TIMEOUT_MS  12000
#define MAX_RC_TARGETS  64

typedef enum { TGT_NONE = 0, TGT_SSH, TGT_RC } target_kind_t;

typedef struct {
	target_kind_t kind;
	char id[96];
	char name[128];
	char host[256];
	char user[128];
	char password[256];
	char key_path[PATH_MAX];
	char token[256];
	int port;
	int has_password;
	int has_key;
} rc_target_t;

typedef struct {
	char target[256];
	double overlap;
	int always_play;
} addon_prefs_t;

static const char *effective_home(void) {
	const char *h = getenv("HOME");
	return (h && *h) ? h : "/var/mobile";
}

static void expand_tilde(const char *in, char *out, size_t n) {
	if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
		snprintf(out, n, "%s%s", effective_home(), in + 1);
	} else {
		snprintf(out, n, "%s", in);
	}
}

static int find_bin(const char *name, char *out, size_t n) {
	static const char *const dirs[] = {
		"/var/jb/usr/bin", "/var/jb/usr/local/bin", "/var/jb/bin",
		"/usr/bin", "/usr/local/bin", "/bin"
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(out, n, "%s/%s", dirs[i], name);
		if (access(out, X_OK) == 0) return 1;
	}
	out[0] = '\0';
	return 0;
}

static const char B64URL[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *b64url_encode(const unsigned char *in, size_t len) {
	size_t cap = ((len + 2) / 3) * 4 + 1;
	char *out = malloc(cap);
	if (!out) oom();
	size_t j = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned)in[i] << 16;
		int rem = (int)(len - i);
		if (rem > 1) v |= (unsigned)in[i + 1] << 8;
		if (rem > 2) v |= (unsigned)in[i + 2];
		out[j++] = B64URL[(v >> 18) & 63];
		out[j++] = B64URL[(v >> 12) & 63];
		if (rem > 1) out[j++] = B64URL[(v >> 6) & 63];
		if (rem > 2) out[j++] = B64URL[v & 63];
	}
	out[j] = '\0';
	return out;
}

static int b64_val(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-' || c == '+') return 62;
	if (c == '_' || c == '/') return 63;
	return -1;
}

static char *b64_decode(const char *in, size_t *out_len) {
	size_t slen = strlen(in);
	unsigned char *out = malloc((slen * 3) / 4 + 4);
	if (!out) oom();
	unsigned val = 0;
	int bits = -8;
	size_t j = 0;
	for (size_t i = 0; i < slen; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '=' || isspace(c)) continue;
		int d = b64_val(c);
		if (d < 0) { free(out); return NULL; }
		val = (val << 6) | (unsigned)d;
		bits += 6;
		if (bits >= 0) {
			out[j++] = (unsigned char)((val >> bits) & 0xFF);
			bits -= 8;
		}
	}
	out[j] = '\0';
	if (out_len) *out_len = j;
	return (char *)out;
}

static char *read_stdin_all(size_t max_bytes) {
	dbuf d = {0};
	char chunk[4096];
	for (;;) {
		ssize_t r = read(STDIN_FILENO, chunk, sizeof chunk);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		if (d.len + (size_t)r > max_bytes) {
			free(d.buf);
			local_fail("input exceeds %zu bytes", max_bytes);
		}
		dbuf_reserve(&d, (size_t)r);
		memcpy(d.buf + d.len, chunk, (size_t)r);
		d.len += (size_t)r;
		d.buf[d.len] = '\0';
	}
	return d.buf ? d.buf : strdup("");
}

static int cf_to_cstr(CFTypeRef ref, char *out, size_t n) {
	out[0] = '\0';
	if (!ref) return 0;
	CFTypeID tid = CFGetTypeID(ref);
	if (tid == CFStringGetTypeID()) {
		if (!CFStringGetCString((CFStringRef)ref, out, (CFIndex)n, kCFStringEncodingUTF8)) {
			out[0] = '\0';
			return 0;
		}
		return out[0] != '\0';
	}
	if (tid == CFNumberGetTypeID()) {
		long long v = 0;
		if (CFNumberGetValue((CFNumberRef)ref, kCFNumberLongLongType, &v)) {
			snprintf(out, n, "%lld", v);
			return 1;
		}
	}
	return 0;
}

static void read_addon_prefs(addon_prefs_t *prefs) {
	memset(prefs, 0, sizeof *prefs);
	prefs->overlap = 1.0;
	prefs->always_play = 0;

	CFStringRef domain = CFSTR(ADDON_DOMAIN);
	CFPreferencesAppSynchronize(domain);

	CFTypeRef v = CFPreferencesCopyAppValue(CFSTR("target"), domain);
	if (v) {
		cf_to_cstr(v, prefs->target, sizeof prefs->target);
		CFRelease(v);
	}
	v = CFPreferencesCopyAppValue(CFSTR("overlap"), domain);
	if (v) {
		if (CFGetTypeID(v) == CFNumberGetTypeID()) {
			double d = 1.0;
			if (CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d) && isfinite(d)) {
				if (d < 0.0) d = 0.0;
				if (d > 5.0) d = 5.0;
				prefs->overlap = d;
			}
		} else if (CFGetTypeID(v) == CFStringGetTypeID()) {
			char buf[64];
			if (cf_to_cstr(v, buf, sizeof buf)) {
				char *end = NULL;
				double d = strtod(buf, &end);
				if (end != buf && isfinite(d)) {
					if (d < 0.0) d = 0.0;
					if (d > 5.0) d = 5.0;
					prefs->overlap = d;
				}
			}
		}
		CFRelease(v);
	}
	v = CFPreferencesCopyAppValue(CFSTR("alwaysPlay"), domain);
	if (v) {
		if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
			prefs->always_play = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
		} else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
			int b = 0;
			if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &b)) prefs->always_play = b != 0;
		}
		CFRelease(v);
	}
}

static int dict_str(CFDictionaryRef d, CFStringRef k, char *out, size_t n) {
	return cf_to_cstr(CFDictionaryGetValue(d, k), out, n);
}

static int dict_int(CFDictionaryRef d, CFStringRef k, int def) {
	CFTypeRef v = CFDictionaryGetValue(d, k);
	if (!v) return def;
	if (CFGetTypeID(v) == CFNumberGetTypeID()) {
		int x = def;
		if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &x)) return x;
	} else if (CFGetTypeID(v) == CFStringGetTypeID()) {
		char buf[32];
		if (cf_to_cstr(v, buf, sizeof buf)) {
			long x = strtol(buf, NULL, 10);
			if (x > 0 && x <= 65535) return (int)x;
		}
	}
	return def;
}

static void parse_ssh_dict(CFDictionaryRef d, rc_target_t *t) {
	memset(t, 0, sizeof *t);
	t->kind = TGT_SSH;
	dict_str(d, CFSTR("id"), t->id, sizeof t->id);
	dict_str(d, CFSTR("name"), t->name, sizeof t->name);
	dict_str(d, CFSTR("host"), t->host, sizeof t->host);
	if (!dict_str(d, CFSTR("user"), t->user, sizeof t->user))
		snprintf(t->user, sizeof t->user, "mobile");
	t->port = dict_int(d, CFSTR("port"), 22);
	if (t->port <= 0 || t->port > 65535) t->port = 22;

	char auth[32] = "";
	dict_str(d, CFSTR("authType"), auth, sizeof auth);
	dict_str(d, CFSTR("keyPath"), t->key_path, sizeof t->key_path);
	dict_str(d, CFSTR("password"), t->password, sizeof t->password);
	if (ieq(auth, "key") && t->key_path[0]) {
		t->has_key = 1;
	} else if (t->password[0]) {
		t->has_password = 1;
	}
	if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", t->host);
}

static void parse_rc_dict(CFDictionaryRef d, rc_target_t *t) {
	memset(t, 0, sizeof *t);
	t->kind = TGT_RC;
	dict_str(d, CFSTR("id"), t->id, sizeof t->id);
	dict_str(d, CFSTR("name"), t->name, sizeof t->name);
	dict_str(d, CFSTR("host"), t->host, sizeof t->host);
	dict_str(d, CFSTR("token"), t->token, sizeof t->token);
	t->port = dict_int(d, CFSTR("port"), 8080);
	if (t->port <= 0 || t->port > 65535) t->port = 8080;
	if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", t->host);
}

static int load_rc_targets(rc_target_t *ssh, int max_ssh, int *n_ssh,
                           rc_target_t *rc, int max_rc, int *n_rc) {
	*n_ssh = 0;
	*n_rc = 0;
	const char *plist_path = getenv("MUSICREMOTE_RC_PLIST");
	if (!plist_path || !*plist_path) plist_path = RC_PLIST_PATH;

	FILE *f = fopen(plist_path, "rb");
	if (!f) return 0;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
	long sz = ftell(f);
	if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(f); return 0; }
	rewind(f);
	unsigned char *buf = malloc((size_t)sz);
	if (!buf) { fclose(f); oom(); }
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);

	CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, buf, (CFIndex)got, kCFAllocatorNull);
	CFPropertyListRef plist = data ? CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL) : NULL;
	if (data) CFRelease(data);
	free(buf);
	if (!plist) return 0;

	if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		CFDictionaryRef root = (CFDictionaryRef)plist;
		CFTypeRef arr = CFDictionaryGetValue(root, CFSTR("sshHosts"));
		if (arr && CFGetTypeID(arr) == CFArrayGetTypeID()) {
			CFIndex count = CFArrayGetCount((CFArrayRef)arr);
			for (CFIndex i = 0; i < count && *n_ssh < max_ssh; i++) {
				CFTypeRef item = CFArrayGetValueAtIndex((CFArrayRef)arr, i);
				if (item && CFGetTypeID(item) == CFDictionaryGetTypeID()) {
					rc_target_t t;
					parse_ssh_dict((CFDictionaryRef)item, &t);
					if (t.host[0]) ssh[(*n_ssh)++] = t;
				}
			}
		}
		arr = CFDictionaryGetValue(root, CFSTR("rcClients"));
		if (arr && CFGetTypeID(arr) == CFArrayGetTypeID()) {
			CFIndex count = CFArrayGetCount((CFArrayRef)arr);
			for (CFIndex i = 0; i < count && *n_rc < max_rc; i++) {
				CFTypeRef item = CFArrayGetValueAtIndex((CFArrayRef)arr, i);
				if (item && CFGetTypeID(item) == CFDictionaryGetTypeID()) {
					rc_target_t t;
					parse_rc_dict((CFDictionaryRef)item, &t);
					if (t.host[0]) rc[(*n_rc)++] = t;
				}
			}
		}
	}
	CFRelease(plist);
	return 1;
}

static int matches_target(const rc_target_t *t, const char *q) {
	return (t->id[0] && ieq(t->id, q)) ||
	       (t->name[0] && ieq(t->name, q)) ||
	       (t->host[0] && ieq(t->host, q));
}

static int looks_ios_ssh(const rc_target_t *t, const rc_target_t *rc, int n_rc) {
	if (ieq(t->user, "mobile")) return 1;
	if (icontains(t->name, "iphone") || icontains(t->name, "ipad") || icontains(t->name, "ipod"))
		return 1;
	if (icontains(t->host, "iphone") || icontains(t->host, "ipad") || icontains(t->host, "ipod"))
		return 1;
	for (int i = 0; i < n_rc; i++) {
		if (ieq(rc[i].host, t->host)) return 1;
	}
	return 0;
}

static const char *trim_copy(const char *s, char *out, size_t n) {
	while (*s && isspace((unsigned char)*s)) s++;
	snprintf(out, n, "%s", s);
	size_t len = strlen(out);
	while (len > 0 && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
	return out;
}

static void parse_direct_ssh(const char *spec, rc_target_t *out) {
	memset(out, 0, sizeof *out);
	out->kind = TGT_SSH;
	out->port = 22;
	snprintf(out->user, sizeof out->user, "mobile");

	const char *at = strchr(spec, '@');
	const char *hostpart = spec;
	if (at && at != spec) {
		size_t ulen = (size_t)(at - spec);
		if (ulen >= sizeof out->user) ulen = sizeof out->user - 1;
		memcpy(out->user, spec, ulen);
		out->user[ulen] = '\0';
		hostpart = at + 1;
	}
	const char *colon = strchr(hostpart, ':');
	if (colon && !strchr(colon + 1, ':')) {
		size_t hlen = (size_t)(colon - hostpart);
		if (hlen >= sizeof out->host) hlen = sizeof out->host - 1;
		memcpy(out->host, hostpart, hlen);
		out->host[hlen] = '\0';
		long p = strtol(colon + 1, NULL, 10);
		if (p > 0 && p <= 65535) out->port = (int)p;
	} else {
		snprintf(out->host, sizeof out->host, "%s", hostpart);
	}
	if (!out->host[0] || out->host[0] == '-' || strchr(out->host, ' ') ||
	    out->user[0] == '-' || strchr(out->user, ' ')) {
		local_fail("invalid target host \"%s\"", spec);
	}
	snprintf(out->name, sizeof out->name, "%s", out->host);
}

static void resolve_target(const char *explicit_target, const addon_prefs_t *prefs,
                           int require_ssh, rc_target_t *out) {
	rc_target_t ssh[MAX_RC_TARGETS], rc[MAX_RC_TARGETS];
	int n_ssh = 0, n_rc = 0;
	load_rc_targets(ssh, MAX_RC_TARGETS, &n_ssh, rc, MAX_RC_TARGETS, &n_rc);

	char qbuf[256];
	const char *raw = (explicit_target && *explicit_target) ? explicit_target : prefs->target;
	trim_copy(raw ? raw : "", qbuf, sizeof qbuf);

	if (qbuf[0] && !ieq(qbuf, "auto")) {
		for (int i = 0; i < n_ssh; i++) {
			if (matches_target(&ssh[i], qbuf)) { *out = ssh[i]; return; }
		}
		if (!require_ssh) {
			for (int i = 0; i < n_rc; i++) {
				if (matches_target(&rc[i], qbuf)) { *out = rc[i]; return; }
			}
		}
		parse_direct_ssh(qbuf, out);
		return;
	}

	for (int i = 0; i < n_ssh; i++) {
		if (looks_ios_ssh(&ssh[i], rc, n_rc)) { *out = ssh[i]; return; }
	}
	if (!require_ssh && n_rc > 0) {
		*out = rc[0];
		return;
	}
	if (require_ssh && n_rc > 0) {
		local_fail("pull requires SSH access to %s (add it under RemoteCompanion > Settings > Integrations > SSH Hosts)",
		           rc[0].name);
	}
	local_fail("no target device configured (add an SSH Host or Remote RC Client in RemoteCompanion, set Transfer Target in Add-ons > MusicRemote, or pass --to <host>)");
}

static int run_ssh_command(const rc_target_t *t, const char *remote_cmd,
                           const char *stdin_data, size_t stdin_len,
                           char **stdout_out, char **stderr_out) {
	*stdout_out = NULL;
	*stderr_out = NULL;

	char ssh_bin[PATH_MAX], sshpass_bin[PATH_MAX];
	if (!find_bin("ssh", ssh_bin, sizeof ssh_bin))
		local_fail("openssh-client is not installed (install openssh-client from Sileo or Zebra)");
	if (t->has_password && !find_bin("sshpass", sshpass_bin, sizeof sshpass_bin))
		local_fail("sshpass is not installed (install sshpass for password SSH auth, or use SSH keys)");

	char key_expanded[PATH_MAX] = "";
	if (t->has_key) expand_tilde(t->key_path, key_expanded, sizeof key_expanded);

	char dest[512], port_str[16];
	if (t->user[0]) snprintf(dest, sizeof dest, "%s@%s", t->user, t->host);
	else snprintf(dest, sizeof dest, "%s", t->host);
	snprintf(port_str, sizeof port_str, "%d", t->port > 0 ? t->port : 22);

	const char *argv[24];
	int argc = 0;
	if (t->has_password) {
		argv[argc++] = sshpass_bin;
		argv[argc++] = "-e";
	}
	argv[argc++] = ssh_bin;
	if (t->has_key && key_expanded[0]) {
		argv[argc++] = "-i";
		argv[argc++] = key_expanded;
	}
	argv[argc++] = "-p";
	argv[argc++] = port_str;
	argv[argc++] = "-o";
	argv[argc++] = "StrictHostKeyChecking=no";
	argv[argc++] = "-o";
	argv[argc++] = "UserKnownHostsFile=/dev/null";
	argv[argc++] = "-o";
	argv[argc++] = "ConnectTimeout=4";
	argv[argc++] = "-o";
	argv[argc++] = "LogLevel=ERROR";
	if (!t->has_password && !t->has_key) {
		argv[argc++] = "-o";
		argv[argc++] = "BatchMode=yes";
	}
	argv[argc++] = dest;
	argv[argc++] = remote_cmd;
	argv[argc] = NULL;

	int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
		for (int i = 0; i < 2; i++) {
			if (in_pipe[i] >= 0) close(in_pipe[i]);
			if (out_pipe[i] >= 0) close(out_pipe[i]);
			if (err_pipe[i] >= 0) close(err_pipe[i]);
		}
		local_fail("failed to create pipes for ssh");
	}

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
	posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
	posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
	posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
	posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
	posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

	/* Build child environment with SSHPASS when needed, without touching parent env. */
	size_t env_count = 0;
	for (char **e = environ; e && *e; e++) env_count++;
	char **child_env = calloc(env_count + 3, sizeof(char *));
	if (!child_env) oom();
	size_t ei = 0;
	for (char **e = environ; e && *e; e++) {
		if (!strncmp(*e, "SSHPASS=", 8) || !strncmp(*e, "PATH=", 5)) continue;
		child_env[ei++] = strdup(*e);
	}
	child_env[ei++] = strdup("PATH=/var/jb/usr/local/bin:/var/jb/usr/bin:/var/jb/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
	if (t->has_password) {
		size_t plen = strlen(t->password) + 9;
		char *sp = malloc(plen);
		if (!sp) oom();
		snprintf(sp, plen, "SSHPASS=%s", t->password);
		child_env[ei++] = sp;
	}
	child_env[ei] = NULL;

	signal(SIGPIPE, SIG_IGN);
	pid_t pid = 0;
	int serr = posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, child_env);
	posix_spawn_file_actions_destroy(&fa);
	for (size_t i = 0; i < ei; i++) free(child_env[i]);
	free(child_env);

	close(in_pipe[0]);
	close(out_pipe[1]);
	close(err_pipe[1]);

	if (serr != 0) {
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(err_pipe[0]);
		*stderr_out = strdup(strerror(serr));
		return -1;
	}

	int fds[3] = { in_pipe[1], out_pipe[0], err_pipe[0] };
	for (int i = 0; i < 3; i++) {
		int fl = fcntl(fds[i], F_GETFL, 0);
		if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
	}
	if (!stdin_data || stdin_len == 0) {
		close(fds[0]);
		fds[0] = -1;
	}

	dbuf ob = {0}, eb = {0};
	size_t in_off = 0;
	int elapsed_ms = 0;

	while (fds[0] >= 0 || fds[1] >= 0 || fds[2] >= 0) {
		struct pollfd pfds[3];
		int idx_map[3];
		nfds_t nfds = 0;
		if (fds[0] >= 0) { pfds[nfds] = (struct pollfd){ fds[0], POLLOUT, 0 }; idx_map[nfds++] = 0; }
		if (fds[1] >= 0) { pfds[nfds] = (struct pollfd){ fds[1], POLLIN, 0 }; idx_map[nfds++] = 1; }
		if (fds[2] >= 0) { pfds[nfds] = (struct pollfd){ fds[2], POLLIN, 0 }; idx_map[nfds++] = 2; }
		if (nfds == 0) break;

		int pr = poll(pfds, nfds, 200);
		if (pr < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pr == 0) {
			elapsed_ms += 200;
			if (elapsed_ms >= SSH_TIMEOUT_MS) {
				kill(pid, SIGKILL);
				break;
			}
			continue;
		}

		for (nfds_t i = 0; i < nfds; i++) {
			int k = idx_map[i];
			short rev = pfds[i].revents;
			if (!rev) continue;
			if (k == 0) {
				if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
					close(fds[0]);
					fds[0] = -1;
					continue;
				}
				ssize_t w = write(fds[0], stdin_data + in_off, stdin_len - in_off);
				if (w > 0) {
					in_off += (size_t)w;
					if (in_off >= stdin_len) { close(fds[0]); fds[0] = -1; }
				} else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
					close(fds[0]);
					fds[0] = -1;
				}
			} else {
				dbuf *dst = (k == 1) ? &ob : &eb;
				char buf[4096];
				for (;;) {
					ssize_t r = read(fds[k], buf, sizeof buf);
					if (r > 0) {
						if (dst->len + (size_t)r < MAX_FILE_BYTES) {
							dbuf_reserve(dst, (size_t)r);
							memcpy(dst->buf + dst->len, buf, (size_t)r);
							dst->len += (size_t)r;
							dst->buf[dst->len] = '\0';
						}
					} else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
						close(fds[k]);
						fds[k] = -1;
						break;
					} else {
						break;
					}
				}
			}
		}
	}

	for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);

	int status = 0;
	waitpid(pid, &status, 0);
	*stdout_out = ob.buf ? ob.buf : strdup("");
	*stderr_out = eb.buf ? eb.buf : strdup("");
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	return -1;
}

static void trim_trailing_ws(char *s) {
	if (!s) return;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static int run_rc_http_receive(const rc_target_t *t, const char *snapshot_json,
                               int force_play, char **err_out) {
	*err_out = NULL;
	char curl_bin[PATH_MAX];
	if (!find_bin("curl", curl_bin, sizeof curl_bin)) {
		*err_out = strdup("curl is not installed");
		return 1;
	}
	if (mkdir_p(dir_path) != 0) {
		*err_out = strdup("cannot create work directory");
		return 1;
	}

	char *b64 = b64url_encode((const unsigned char *)snapshot_json, strlen(snapshot_json));
	dbuf cmd = {0};
	dbuf_add(&cmd, "exec musicremote receive --json ");
	dbuf_add(&cmd, force_play ? "--play " : "--pause ");
	dbuf_add(&cmd, "--b64 ");
	dbuf_add(&cmd, b64);
	free(b64);

	dbuf body = {0};
	dbuf_add(&body, "{\"command\":");
	dbuf_add_escaped(&body, cmd.buf);
	dbuf_add(&body, "}");
	free(cmd.buf);

	char body_path[PATH_MAX], cfg_path[PATH_MAX], out_path[PATH_MAX], url[512];
	unsigned r = (unsigned)arc4random();
	snprintf(body_path, sizeof body_path, "%s/rc_body_%u.json", dir_path, r);
	snprintf(cfg_path, sizeof cfg_path, "%s/rc_cfg_%u.txt", dir_path, r);
	snprintf(out_path, sizeof out_path, "%s/rc_resp_%u.txt", dir_path, r);
	snprintf(url, sizeof url, "http://%s:%d/api/command", t->host, t->port > 0 ? t->port : 8080);

	int fd = open(body_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, body.buf, body.len) != (ssize_t)body.len) {
		if (fd >= 0) close(fd);
		free(body.buf);
		unlink(body_path);
		*err_out = strdup("cannot write request payload");
		return 1;
	}
	close(fd);
	free(body.buf);

	dbuf cfg = {0};
	dbuf_add(&cfg, "header = \"Content-Type: application/json\"\n");
	if (t->token[0]) {
		dbuf_add(&cfg, "header = \"Authorization: Bearer ");
		for (const char *p = t->token; *p; p++) {
			if (*p == '"' || *p == '\\') dbuf_addc(&cfg, '\\');
			dbuf_addc(&cfg, *p);
		}
		dbuf_add(&cfg, "\"\n");
	}
	fd = open(cfg_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, cfg.buf, cfg.len) != (ssize_t)cfg.len) {
		if (fd >= 0) close(fd);
		free(cfg.buf);
		unlink(body_path);
		unlink(cfg_path);
		*err_out = strdup("cannot write curl config");
		return 1;
	}
	close(fd);
	free(cfg.buf);

	char *argv[] = {
		curl_bin, (char *)"-sS", (char *)"--max-time", (char *)"6",
		(char *)"-w", (char *)"%{http_code}",
		(char *)"-K", cfg_path,
		(char *)"--data-binary", NULL,
		(char *)"-o", out_path,
		url, NULL
	};
	char at_body[PATH_MAX + 2];
	snprintf(at_body, sizeof at_body, "@%s", body_path);
	argv[9] = at_body;

	int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
	if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
		unlink(body_path);
		unlink(cfg_path);
		*err_out = strdup("pipe failed");
		return 1;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
	posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
	posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
	posix_spawn_file_actions_addclose(&fa, err_pipe[1]);

	pid_t pid = 0;
	int serr = posix_spawn(&pid, curl_bin, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(out_pipe[1]);
	close(err_pipe[1]);

	char code_buf[32] = "", curl_err[512] = "";
	if (serr == 0) {
		ssize_t rn = read(out_pipe[0], code_buf, sizeof code_buf - 1);
		if (rn > 0) code_buf[rn] = '\0';
		rn = read(err_pipe[0], curl_err, sizeof curl_err - 1);
		if (rn > 0) curl_err[rn] = '\0';
		int st = 0;
		waitpid(pid, &st, 0);
	}
	close(out_pipe[0]);
	close(err_pipe[0]);

	char *resp_body = read_text_file(out_path);
	unlink(body_path);
	unlink(cfg_path);
	unlink(out_path);

	long http_code = strtol(code_buf, NULL, 10);
	if (serr != 0 || http_code == 0) {
		trim_trailing_ws(curl_err);
		char msg[768];
		snprintf(msg, sizeof msg, "could not reach %s (%s)", t->name,
		         curl_err[0] ? curl_err : "check Web UI is enabled");
		*err_out = strdup(msg);
		free(resp_body);
		return 1;
	}
	if (http_code == 403) {
		char msg[512];
		if (resp_body && strstr(resp_body, "disabled")) {
			snprintf(msg, sizeof msg, "RemoteCompanion Web UI is disabled on %s", t->name);
		} else {
			snprintf(msg, sizeof msg,
			         "Remote RC client \"%s\" requires its Web UI API token (or add it under SSH Hosts)",
			         t->name);
		}
		*err_out = strdup(msg);
		free(resp_body);
		return 1;
	}
	if (http_code < 200 || http_code >= 300) {
		char msg[512];
		snprintf(msg, sizeof msg, "%s returned HTTP %ld", t->name, http_code);
		*err_out = strdup(msg);
		free(resp_body);
		return 1;
	}
	free(resp_body);
	return 0;
}

static int resolve_local_track(sqlite3 *h, const char *title, const char *artist,
                               const char *album, long long *out_store,
                               sqlite3_int64 *out_pid) {
	*out_store = 0;
	*out_pid = 0;
	if (!h || !title || !*title) return 0;
	ensure_schema(h);
	const char *tcol = sch.item_title ? "i.title" : "e.title";
	const char *tj = sch.item_title ? "" : " LEFT JOIN item_extra e ON e.item_pid = i.item_pid";
	const char *acol = sch.item_artist ? "ia.item_artist" : sch.person_name ? "p.name" : "''";
	const char *aj = sch.item_artist ? " LEFT JOIN item_artist ia ON ia.item_artist_pid = i.item_artist_pid"
	                                 : sch.person_name ? " LEFT JOIN person p ON p.person_pid = i.item_artist_pid" : "";
	char sql[MAX_SQL];
	snprintf(sql, sizeof sql,
	         "SELECT i.item_pid,"
	         " coalesce(nullif(s.subscription_store_item_id,0), s.asset_store_item_id, 0),"
	         " coalesce(nullif(%s,''), aa.album_artist, ''),"
	         " coalesce(a.album, '')"
	         " FROM item i%s%s"
	         " LEFT JOIN item_store s ON s.item_pid = i.item_pid"
	         " LEFT JOIN album a ON a.album_pid = i.album_pid"
	         " LEFT JOIN album_artist aa ON aa.album_artist_pid = i.album_artist_pid"
	         " WHERE i.media_type = 8 AND %s = ?1 COLLATE NOCASE"
	         " LIMIT 32",
	         acol, tj, aj, tcol);

	sqlite3_stmt *st = prepare(h, sql);
	sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
	int best_score = -1;
	while (sqlite3_step(st) == SQLITE_ROW) {
		sqlite3_int64 pid = sqlite3_column_int64(st, 0);
		long long sid = (long long)sqlite3_column_int64(st, 1);
		const char *ra = (const char *)sqlite3_column_text(st, 2);
		const char *ral = (const char *)sqlite3_column_text(st, 3);
		int score = 0;
		if (artist && *artist && ra && ieq(ra, artist)) score += 2;
		else if (artist && *artist && ra && icontains(ra, artist)) score += 1;
		if (album && *album && ral && ieq(ral, album)) score += 2;
		if (sid > 0) score += 1;
		if (score > best_score) {
			best_score = score;
			*out_pid = pid;
			*out_store = sid;
		}
	}
	sqlite3_finalize(st);
	return best_score >= 0;
}

static int apply_handoff_payload(const char *snap, int force_play, int as_json, char **resp_out) {
	*resp_out = NULL;
	char state[32] = "", shuffle[32] = "off", repeat[32] = "none";
	char title[256] = "", artist[256] = "", album[256] = "", top_store[24] = "";
	double elapsed = 0.0, qidx_d = -1.0;

	json_str(snap, "state", state, sizeof state);
	json_str(snap, "shuffle", shuffle, sizeof shuffle);
	json_str(snap, "repeat", repeat, sizeof repeat);
	json_str(snap, "title", title, sizeof title);
	json_str(snap, "artist", artist, sizeof artist);
	json_str(snap, "album", album, sizeof album);
	json_str(snap, "storeID", top_store, sizeof top_store);
	json_num(snap, "elapsed", &elapsed);
	json_num(snap, "queueIndex", &qidx_d);
	if (elapsed < 0.0 || !isfinite(elapsed)) elapsed = 0.0;

	int should_play = 1;
	if (force_play >= 0) {
		should_play = force_play;
	} else {
		int explicit_play = 0;
		if (json_bool(snap, "play", &explicit_play)) should_play = explicit_play;
		else should_play = !strcmp(state, "playing") ? 1 : 0;
	}

	int qidx = (qidx_d >= 0.0) ? (int)qidx_d : -1;
	long long store_ids_arr[MAX_STORE_IDS];
	int n_store = 0, start_store_pos = -1;
	sqlite3_int64 pid_arr[MAX_STORE_IDS];
	int n_pid = 0, start_pid_pos = -1;
	sqlite3 *local_db = NULL;

	const char *queue = json_value(snap, "queue");
	if (queue && *queue == '[') {
		const char *p = queue + 1;
		int idx = 0;
		for (;;) {
			p = skip_ws(p);
			if (*p == ',') { p++; continue; }
			if (*p != '{') break;
			const char *end = skip_value(p);
			if (!end) break;

			char qsid[24] = "", qt[256] = "", qa[256] = "", qal[256] = "";
			json_str(p, "storeID", qsid, sizeof qsid);
			long long sid = qsid[0] ? strtoll(qsid, NULL, 10) : 0;
			sqlite3_int64 pid = 0;

			if (sid <= 0) {
				json_str(p, "title", qt, sizeof qt);
				json_str(p, "artist", qa, sizeof qa);
				json_str(p, "album", qal, sizeof qal);
				if (qt[0]) {
					if (!local_db) local_db = open_db();
					resolve_local_track(local_db, qt, qa, qal, &sid, &pid);
				}
			}

			if (sid > 0 && n_store < MAX_STORE_IDS) {
				if (idx == qidx) start_store_pos = n_store;
				store_ids_arr[n_store++] = sid;
			} else if (pid != 0 && n_pid < MAX_STORE_IDS) {
				if (idx == qidx) start_pid_pos = n_pid;
				pid_arr[n_pid++] = pid;
			}
			idx++;
			p = end;
		}
	}

	long long top_sid = top_store[0] ? strtoll(top_store, NULL, 10) : 0;
	if (n_store == 0 && n_pid == 0) {
		sqlite3_int64 pid = 0;
		if (top_sid <= 0 && title[0]) {
			if (!local_db) local_db = open_db();
			resolve_local_track(local_db, title, artist, album, &top_sid, &pid);
		}
		if (top_sid > 0) {
			store_ids_arr[0] = top_sid;
			n_store = 1;
			start_store_pos = 0;
		} else if (pid != 0) {
			pid_arr[0] = pid;
			n_pid = 1;
			start_pid_pos = 0;
		}
	}
	if (local_db) close_db(local_db);

	if (n_store == 0 && n_pid == 0) {
		local_fail("that song is not playable on this device");
	}

	dbuf args = {0};
	dbuf_add(&args, "{");
	if (n_store > 0) {
		int slice_from = 0;
		if (start_store_pos < 0 && top_sid > 0) {
			start_store_pos = index_of_id(store_ids_arr, n_store, top_sid);
		}
		if (start_store_pos < 0) start_store_pos = 0;
		long long start_id = store_ids_arr[start_store_pos];
		/* startItemID matches the first occurrence; slice if the same track appeared earlier. */
		for (int i = 0; i < start_store_pos; i++) {
			if (store_ids_arr[i] == start_id) { slice_from = start_store_pos; break; }
		}
		dbuf_add(&args, "\"ids\":[");
		for (int i = slice_from; i < n_store; i++) {
			dbuf_addf(&args, "%s\"%lld\"", i > slice_from ? "," : "", store_ids_arr[i]);
		}
		dbuf_addf(&args, "],\"start\":\"%lld\",", start_id);
	} else {
		if (start_pid_pos < 0) start_pid_pos = 0;
		char pbuf[24];
		dbuf_add(&args, "\"pids\":[");
		for (int i = 0; i < n_pid; i++) {
			pid_string(pid_arr[i], pbuf, sizeof pbuf);
			dbuf_addf(&args, "%s\"%s\"", i ? "," : "", pbuf);
		}
		pid_string(pid_arr[start_pid_pos], pbuf, sizeof pbuf);
		dbuf_addf(&args, "],\"start\":\"%s\",", pbuf);
	}
	dbuf_addf(&args, "\"elapsed\":%.3f,\"play\":%s,\"shuffle\":",
	          elapsed, should_play ? "true" : "false");
	dbuf_add_escaped(&args, shuffle[0] ? shuffle : "off");
	dbuf_add(&args, ",\"repeat\":");
	dbuf_add_escaped(&args, repeat[0] ? repeat : "none");
	dbuf_add(&args, "}");

	request_t req = { "handoff", args.buf };
	int rc = exec_request(&req, as_json, resp_out);
	free(args.buf);
	return rc;
}

static int cmd_snapshot(int as_json) {
	request_t req = { "snapshot", "{}" };
	char *data = NULL;
	int rc = exec_request(&req, as_json, &data);
	if (rc != 0) return rc;
	if (as_json) {
		printf("%s\n", data);
	} else {
		print_status(data);
	}
	free(data);
	return 0;
}

static int cmd_receive(int ac, char **av, int as_json) {
	int force_play = -1;
	const char *b64_arg = NULL, *raw_arg = NULL;
	for (int i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--play")) force_play = 1;
		else if (!strcmp(av[i], "--pause")) force_play = 0;
		else if (!strcmp(av[i], "--match")) force_play = -1;
		else if (!strcmp(av[i], "--b64")) {
			if (++i >= ac) return usage_error("--b64 needs a payload");
			b64_arg = av[i];
		} else if (!strcmp(av[i], "--data")) {
			if (++i >= ac) return usage_error("--data needs JSON");
			raw_arg = av[i];
		} else {
			return usage_error("receive takes --play, --pause, --match, --b64 or --data");
		}
	}

	char *owned = NULL;
	const char *snap = NULL;
	if (b64_arg) {
		owned = b64_decode(b64_arg, NULL);
		if (!owned) local_fail("invalid base64 handoff payload");
		snap = owned;
	} else if (raw_arg) {
		snap = raw_arg;
	} else {
		owned = read_stdin_all(MAX_FILE_BYTES);
		snap = owned;
	}

	const char *ws = skip_ws(snap);
	if (*ws != '{') {
		free(owned);
		local_fail("handoff payload is not a JSON object");
	}

	char *resp = NULL;
	int rc = apply_handoff_payload(ws, force_play, as_json, &resp);
	free(owned);
	if (rc != 0) return rc;
	if (as_json) printf("%s\n", resp);
	else print_status(resp);
	free(resp);
	return 0;
}

static char *inject_play_flag(const char *snap, int should_play) {
	const char *p = skip_ws(snap);
	if (*p != '{') return strdup(snap);
	dbuf d = {0};
	dbuf_addf(&d, "{\"play\":%s,", should_play ? "true" : "false");
	dbuf_add(&d, p + 1);
	return d.buf;
}

static void print_transfer_result(const char *verb, const char *peer, const char *title,
                                  const char *artist, int played, int paused_peer, int as_json) {
	if (as_json) {
		dbuf d = {0};
		dbuf_add(&d, "{\"ok\":true,\"target\":");
		dbuf_add_escaped(&d, peer);
		dbuf_add(&d, ",\"title\":");
		dbuf_add_escaped(&d, title);
		dbuf_add(&d, ",\"artist\":");
		dbuf_add_escaped(&d, artist);
		dbuf_addf(&d, ",\"played\":%s,\"pausedSource\":%s}",
		          played ? "true" : "false", paused_peer ? "true" : "false");
		printf("%s\n", d.buf);
		free(d.buf);
		return;
	}
	printf("%s %s", verb, peer);
	if (title && *title) printf(" — %s", title);
	if (!played) printf(" (paused)");
	putchar('\n');
}

static int cmd_pull(int ac, char **av, int as_json) {
	addon_prefs_t prefs;
	read_addon_prefs(&prefs);
	char target_buf[256] = "";
	double overlap = prefs.overlap;
	int force_play = -1;

	for (int i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--from") || !strcmp(av[i], "--to")) {
			if (++i >= ac) return usage_error("--from needs a device name or host");
			snprintf(target_buf, sizeof target_buf, "%s", av[i]);
		} else if (!strcmp(av[i], "--overlap")) {
			if (++i >= ac) return usage_error("--overlap needs seconds");
			char *end = NULL;
			double v = strtod(av[i], &end);
			if (end == av[i] || *end || !isfinite(v) || v < 0.0 || v > 5.0)
				return usage_error("--overlap must be between 0 and 5");
			overlap = v;
		} else if (!strcmp(av[i], "--play")) {
			force_play = 1;
		} else if (!strcmp(av[i], "--pause")) {
			force_play = 0;
		} else if (av[i][0] == '-' && av[i][1] == '-') {
			return usage_error("unknown flag for pull");
		} else if (query_add(target_buf, sizeof target_buf, av[i]) != 0) {
			return usage_error("target name is too long");
		}
	}

	rc_target_t src;
	resolve_target(target_buf[0] ? target_buf : NULL, &prefs, 1, &src);

	char *snap = NULL, *serr = NULL;
	int ssh_rc = run_ssh_command(&src, "musicremote snapshot --json", NULL, 0, &snap, &serr);
	if (ssh_rc != 0) {
		trim_trailing_ws(serr);
		trim_trailing_ws(snap);
		const char *why = (serr && *serr) ? serr : (snap && *snap) ? snap : "SSH command failed";
		char msg[768];
		snprintf(msg, sizeof msg, "%s: %s", src.name, why);
		free(snap);
		free(serr);
		local_fail("%s", msg);
	}
	free(serr);

	char state[32] = "", title[256] = "", artist[256] = "";
	json_str(snap, "state", state, sizeof state);
	json_str(snap, "title", title, sizeof title);
	json_str(snap, "artist", artist, sizeof artist);
	int was_playing = !strcmp(state, "playing");
	int target_play = force_play >= 0 ? force_play : (was_playing || prefs.always_play);

	char *resp = NULL;
	int lrc = apply_handoff_payload(snap, target_play, as_json, &resp);
	free(snap);
	if (lrc != 0) return lrc;
	free(resp);

	if (was_playing) {
		if (overlap > 0.0) usleep((useconds_t)(overlap * 1000000.0));
		char *pout = NULL, *perr = NULL;
		(void)run_ssh_command(&src, "musicremote pause", NULL, 0, &pout, &perr);
		free(pout);
		free(perr);
	}

	print_transfer_result("Pulled from", src.name, title, artist, target_play, was_playing, as_json);
	return 0;
}

static int cmd_transfer(int ac, char **av, int as_json) {
	addon_prefs_t prefs;
	read_addon_prefs(&prefs);
	char target_buf[256] = "";
	double overlap = prefs.overlap;
	int force_play = -1;

	for (int i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--from")) {
			/* Delegate transfer --from <source> cleanly to pull. */
			return cmd_pull(ac, av, as_json);
		} else if (!strcmp(av[i], "--to")) {
			if (++i >= ac) return usage_error("--to needs a device name or host");
			snprintf(target_buf, sizeof target_buf, "%s", av[i]);
		} else if (!strcmp(av[i], "--overlap")) {
			if (++i >= ac) return usage_error("--overlap needs seconds");
			char *end = NULL;
			double v = strtod(av[i], &end);
			if (end == av[i] || *end || !isfinite(v) || v < 0.0 || v > 5.0)
				return usage_error("--overlap must be between 0 and 5");
			overlap = v;
		} else if (!strcmp(av[i], "--play")) {
			force_play = 1;
		} else if (!strcmp(av[i], "--pause")) {
			force_play = 0;
		} else if (av[i][0] == '-' && av[i][1] == '-') {
			return usage_error("expected --to, --from, --overlap, --play or --pause");
		} else if (query_add(target_buf, sizeof target_buf, av[i]) != 0) {
			return usage_error("target name is too long");
		}
	}

	rc_target_t dst;
	resolve_target(target_buf[0] ? target_buf : NULL, &prefs, 0, &dst);

	request_t sreq = { "snapshot", "{}" };
	char *snap = NULL;
	int rc = exec_request(&sreq, as_json, &snap);
	if (rc != 0) return rc;

	char state[32] = "", title[256] = "", artist[256] = "";
	json_str(snap, "state", state, sizeof state);
	json_str(snap, "title", title, sizeof title);
	json_str(snap, "artist", artist, sizeof artist);
	int was_playing = !strcmp(state, "playing");
	int target_play = force_play >= 0 ? force_play : (was_playing || prefs.always_play);

	char *payload = inject_play_flag(snap, target_play);
	free(snap);

	if (dst.kind == TGT_SSH) {
		char *rout = NULL, *rerr = NULL;
		const char *rcmd = target_play
			? "musicremote receive --json --play"
			: "musicremote receive --json --pause";
		int ssh_rc = run_ssh_command(&dst, rcmd, payload, strlen(payload), &rout, &rerr);
		free(payload);
		if (ssh_rc != 0) {
			trim_trailing_ws(rerr);
			trim_trailing_ws(rout);
			const char *why = (rerr && *rerr) ? rerr : (rout && *rout) ? rout : "transfer failed on target";
			char msg[768];
			snprintf(msg, sizeof msg, "%s: %s", dst.name, why);
			free(rout);
			free(rerr);
			local_fail("%s", msg);
		}
		free(rout);
		free(rerr);
		if (was_playing && overlap > 0.0) {
			usleep((useconds_t)(overlap * 1000000.0));
		}
	} else {
		char *herr = NULL;
		int hrc = run_rc_http_receive(&dst, payload, target_play, &herr);
		free(payload);
		if (hrc != 0) {
			char msg[768];
			snprintf(msg, sizeof msg, "%s", herr ? herr : "Remote RC request failed");
			free(herr);
			local_fail("%s", msg);
		}
		/* /api/command returns immediately on accept; wait for the target to buffer and start. */
		if (was_playing) {
			double wait_s = overlap + 0.8;
			if (wait_s > 0.0) usleep((useconds_t)(wait_s * 1000000.0));
		}
	}

	if (was_playing) {
		request_t preq = { "pause", "{}" };
		char *pdata = NULL;
		if (exec_request(&preq, 0, &pdata) == 0) free(pdata);
	}

	print_transfer_result("Sent to", dst.name, title, artist, target_play, was_playing, as_json);
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
	if (!strcmp(av[0], "love")) return cmd_love(ac, av, as_json);
	if (!strcmp(av[0], "snapshot")) {
		if (ac != 1) return usage_error("this command takes no arguments");
		return cmd_snapshot(as_json);
	}
	if (!strcmp(av[0], "receive")) return cmd_receive(ac, av, as_json);
	if (!strcmp(av[0], "transfer")) return cmd_transfer(ac, av, as_json);
	if (!strcmp(av[0], "pull")) return cmd_pull(ac, av, as_json);
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

		/* An album name queues the catalogue's full running order, so tracks
		 * that were never added to the library are included too. */
		if (!strcmp(a, "album") && n > 0) {
			long long full[MAX_STORE_IDS];
			int full_n = catalog_album_ids(ids[0], full, MAX_STORE_IDS);
			if (full_n > 0) {
				memcpy(ids, full, sizeof(long long) * (size_t)full_n);
				n = full_n;
			}
		}

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
