# MusicRemote protocol

One transport, two sides. The CLI writes a request file, posts a Darwin
notification, the tweak injected into SpringBoard picks it up, acts, writes a
response file and posts a second notification. No Mach services, no
entitlements, no extra libraries.

## Paths (on device)

    /var/mobile/Media/.musicremote/req.json      request, written by the CLI
    /var/mobile/Media/.musicremote/req.json.tmp  staging file for the rename
    /var/mobile/Media/.musicremote/res.json      response, written by the tweak
    /var/mobile/Media/.musicremote/lock          flock target, one call at a time

## Notifications

    com.chris.musicremote.req    CLI -> tweak
    com.chris.musicremote.res    tweak -> CLI

Post with `kCFNotificationDeliverImmediately`, no object, no userInfo.

## Request

```json
{"id":"0F3A...","action":"queueAlbum","args":{...}}
```

`id` is a 16-char hex string, unique per call. The tweak echoes it back so the
CLI can ignore a stale response from an earlier call.

### Actions

| action | args | notes |
|---|---|---|
| `status` | none | now playing snapshot |
| `play` / `pause` / `toggle` | none | |
| `next` / `prev` | none | |
| `queueByStore` | `ids`, `mode` | **preferred.** Apple Music catalog IDs, in play order, decimals as strings |
| `queueByID` | `property`, `id`, `mode` | fallback when a track has no catalog ID; unreliable, see below |
| `queueAlbum` | `query`, `mode` | compatibility only |
| `queueArtist` | `query`, `mode` | compatibility only |
| `queueSong` | `query`, `artist`, `mode` | compatibility only |
| `search` | `query`, `limit` | tweak-side, sees only downloaded items |
| `library` | none | tweak-side counts, sees only downloaded items |
| `volume` | `value` | 0.0-1.0 |

`mode`: `replace` (default, queues and starts playing), `append` (queues
without changing playback), `next` (inserts directly after the now playing
item, so it starts when the current track ends).

### Why store IDs

`MPMediaQuery` only enumerates downloaded items: 132 of 770 songs on the test
device. Predicate filters are ignored in a queue descriptor, so an album query
queues the whole visible library. `MPMusicPlayerStoreQueueDescriptor` with
catalog IDs plays local and cloud items alike and needs no library access.

The CLI resolves names against `MediaLibrary.sqlitedb`:

```sql
select coalesce(nullif(s.subscription_store_item_id,0), s.asset_store_item_id)
from item i join item_store s on s.item_pid = i.item_pid
where i.album_pid = ? and i.media_type = 8
order by i.disc_number, i.track_number;
```

If that returns nothing the CLI falls back to `queueByID`. Capped at 600 IDs
per request.

Persistent IDs in `queueByID` are 64-bit, so they travel as decimal strings and
negative SQLite values are reinterpreted as unsigned.

## Response

```json
{"id":"0F3A...","ok":true,"data":{...}}
{"id":"0F3A...","ok":false,"error":"no album matched \"kind of blu\""}
```

`status` data:

```json
{"state":"playing","title":"So What","artist":"Miles Davis",
 "album":"Kind of Blue","index":1,"total":5,
 "elapsed":12.4,"duration":545.0,
 "albumID":"1421350079176516109","albumArtistID":"4273702906016223495",
 "trackID":"5481794911435167","disc":1,"track":1}
```

The three ID fields are 64-bit and therefore decimal strings. The CLI uses them
for the `current` commands, so it can queue the right album without matching on
names. `albumID` is the album of the now-playing item, `albumArtistID` its
album artist, `trackID` the item itself.

### Current-item queueing

The CLI turns a `status` response into one of these:

- `current album` replaces the queue with the playing album, starting at the
  current track.
- `current album --from-start` starts at track 1 instead.
- `current album --rest` appends only the tracks after the current one.
- `current album --next` inserts those tracks directly after the now playing
  item, so they start when the current song ends.
- `current album --append` appends the whole album, playback untouched.
- `current artist` replaces the queue with every track by the album artist,
  starting at the current track.
- `current artist --next` inserts the artist's other tracks after the current
  song.
- `current artist --append` appends them instead.

They resolve IDs through the database exactly like the name commands, so
`queueByStore` stays the only transport.

`queueByStore` data adds `queued`, `mode`, and the same status fields.
`queueByID` adds `mode` and `id` but no `queued`.
`search` data: `{"count":3,"items":[{"title":..,"artist":..,"album":..}]}`

Queueing always sets shuffle off, so an album plays in its own order.

The CLI keeps the now playing track out of any album or artist queue it sends
with `next` or `append`, so the current song is never queued behind itself.
That also covers the case of queueing the album whose first track is playing.

## Rules

- Write `*.tmp`, then rename over the real name. Never partially write.
- The tweak unlinks `req.json` before acting, so a duplicate notification
  cannot replay a command.
- CLI polls `res.json` every 50 ms, gives up after 8 s (the first call after
  Music starts can take about 5 s), then reports that SpringBoard did not
  answer.
- One request in flight at a time. The CLI holds the lock file.
- Name matching lives in the CLI, against the database. The tweak's query
  actions exist only for compatibility.
