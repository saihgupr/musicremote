# MusicRemote

Drive the iOS 15 Music app from the command line: queue an album or an artist,
play, pause, skip, and report what is playing.

Two pieces, one package:

- `MusicRemote.dylib` is injected into SpringBoard. It listens for a Darwin
  notification, reads a request file, talks to `MPMusicPlayerController`'s
  system player (which is Music.app), and writes a response file.
- `musicremote` is a small CLI on the device that resolves names against the
  iTunes library database and sends the request.

`rc music ...` (RemoteCompanion) runs the same CLI through a **RemoteCompanion
add-on** that ships in this package. RemoteCompanion needs no code changes for
it: the file is `layout/Library/RemoteCompanion/AddOns/music.json`, installed to
`/var/jb/Library/RemoteCompanion/AddOns/`, and RemoteCompanion picks it up
without a respring. `rc addons` lists it as `MusicRemote (music)`.

The add-on is deliberately small, because the phone UI is buttons. It exposes
play, pause, play or pause, next, previous, queue this artist, queue this album,
add this artist to the queue and add this album to the queue. One condition,
player state (Playing / Paused / Stopped), covers automations. Everything else
(name queues, search, library counts, volume, queue variants) stays in the CLI,
where it is actually useful. Check the condition with
`rc addons check addon:music:state`.

## Why it is built this way

Inside Music.app the public player API deadlocks: it is the system player's
owner, so a call from its own main thread waits on itself. SpringBoard is a
separate process, always running, and drives Music.app the way a normal app
would.

`MPMediaQuery` only enumerates downloaded tracks (132 of 770 on this phone) and
ignores predicate filters in queue descriptors, so name lookup happens in the
CLI against `MediaLibrary.sqlitedb`, and playback is queued by Apple Music
catalog IDs (`subscription_store_item_id`). That plays cloud items too.

## Requirements

- Jailbroken (Dopamine) rootless iOS 15 device, ElleKit injection
- Theos at `/opt/theos` to build
- RemoteCompanion installed if you want `rc music`

## Build and install

```sh
THEOS=/opt/theos make package
RC_PASSWORD='<mobile password>' ./deploy.sh iphone
RC_PASSWORD='<mobile password>' ./deploy.sh ipad
```

`musicremote` installs into `/usr/bin` (on the device, `/var/jb/usr/bin`), the
same directory as `rc`. That keeps it on the default PATH, so a bare
`musicremote` works from non-login shells like Keyboard Maestro's SSH action.

`deploy.sh` installs the newest `.deb`, removes the old copy from
`/usr/local/bin`, then resprings SpringBoard so the dylib loads. Use the shared
device lock for any install:

```sh
~/.config/opencode/scripts/device-lock.sh run "MusicRemote deploy" 600 -- ./deploy.sh iphone
```

## Usage

On the device over ssh:

```sh
musicremote status
musicremote play | pause | toggle | next | prev
musicremote album Samorost 3
musicremote album "Best of Martin Denny's Exotica" --append
musicremote artist "Rabih Abou-Khalil"
musicremote song "Remembering Machghara"
musicremote queue album                 # rest of the playing album, plays next
musicremote queue album --append        # rest of the album, at the end
musicremote queue album --from-start    # replace the queue, from track 1
musicremote queue artist                # this artist's other tracks, next
musicremote queue artist --append       # at the end
musicremote search "samorost" --limit 5
musicremote library
musicremote state                       # one word, for scripts and conditions
musicremote count songs                 # or albums, artists
musicremote love                        # love the playing song
musicremote love off                    # take it back
musicremote love status                 # loved or not loved
musicremote --json status
```

Through RemoteCompanion, same commands after `rc music`:

```sh
rc music status
rc music album Kind of Blue
rc music queue artist
```

Quotes are optional. All words after the subcommand form the query, so
`album Kind of Blue` and `album "Kind of Blue"` are the same.

`queue album` queues the whole album in album order, leaving the playing song
out so nothing is heard twice, and starts it when the current song ends.
`--rest` is the narrower one: only the tracks after the current song.
`--append` puts the album at the end instead, and `--from-start` replaces the
queue and starts at track 1.

An album is resolved through Apple's public iTunes lookup, not just the local
library, so an album with only a few songs added still plays end to end. That
needs network; without it the library's own tracks are used.

## Limits

- 600 tracks per request (the wire body is built dynamically).
- `status` right after a queue change can be one step stale; the player
  transitions asynchronously, ask again a second later.
- Track matching is ASCII-folded. `brucke` does not find `Brücke`.
- The tweak's own `queueAlbum` / `queueArtist` / `queueByID` actions remain for
  compatibility but only see downloaded items. Do not use them.

## Debugging

The tweak writes three files next to the protocol files:

- `/var/mobile/Media/.musicremote/loaded.txt` - the dylib loaded
- `/var/mobile/Media/.musicremote/notified.txt` - a notification arrived
- `/var/mobile/Media/.musicremote/trace.log` - per-request steps

If `notified.txt` updates but `trace.log` does not, the process is stuck. If
neither updates, SpringBoard does not have the dylib: respring.
