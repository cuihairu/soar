# Architecture overview

Soar is a single process: one binary hosts the desktop UI, the playback core,
the media services, the P2P bridge and the casting control point. Everything
the process talks to — a DLNA receiver, a torrent swarm, a byte-range HTTP
host, an optional API — sits outside the box.

<picture>
  <source media="(max-width: 640px)" srcset="/architecture/system-en-mobile.svg">
  <img src="/architecture/system-en.svg" alt="Soar playback process and network peers: entry points, playback core, media services, P2P bridge, casting control point, and the external peers they talk to" width="100%">
</picture>

Line styles: solid arrows carry media, control commands and results; dashed
arrows carry discovery and subscription traffic (SSDP discovery, GENA NOTIFY).

## The process

`soarAppMain` is the shared entry point for the console build (`soar`) and the
Windows GUI build (`soarw.exe`); it parses flags, chooses a backend and hands
over. `player_window` owns the SDL2 window, the ImGui surface, the on-screen
controls, the overlays and the present path. `ui_state` is the pure logic
around it — OSC auto-hide, recent files, folder listing, the key map — and
`library` keeps watched folders and resume positions in `library.txt`.

## Playback core

`Player` is a thin facade with one event callback; it owns no threads. It
delegates to an `IBackend`, which has two implementations: `FFmpegBackend`,
the only FFmpeg build, owning the decode thread, hardware decoding, the
HDR→SDR tone map, audio output and screenshots; and `NullBackend`, a
thread-less test double used by the no-FFmpeg build and by the test suite.
Both draw frames through the same contract, so the UI never learns which
backend it is talking to.

## Media services

`HttpCache` is a custom FFmpeg `AVIOContext`: 256 KiB blocks tracked by a
one-bit-per-block bitmap, wired in only for `http://` URLs and only from
`FFmpegBackend`, with no eviction policy. `Subtitles + Ass` covers the sidecar
and catalog providers, the tolerant parser, the optional translator, and
`AssRenderer` — the only libass caller, whose canvas the UI composites over the
video. `TorrentStream` uses libtorrent purely as a download kernel and serves
the file it downloaded over a `127.0.0.1` byte-range HTTP bridge, so the
backend just opens an ordinary URL and learns nothing about torrents.

## Casting

`CastSession` is a DLNA control point: it discovers receivers over SSDP,
talks AVTransport over SOAP, and subscribes to GENA events. Two local HTTP
servers round it out — the event server, which the receiver's NOTIFY callbacks
hit, and the transcode server, which the receiver pulls an MPEG-TS stream from.
The dashed arrow is that subscription and its NOTIFY replies; the solid arrow
is the control traffic plus the media flowing out.

## Disk and optional endpoints

Local disk holds the media files, the cache blocks and `library.txt`. The
subtitle catalog, the metadata search and the translation API are optional
endpoints injected through the environment; with none of them configured the
player still plays, just without those features.

## Where the picture and the code disagree

The diagram follows the code. Three things in the older documents do not match
it, and this page reflects the code:

- `mvp.md` describes the cache and download layer as standalone components that
  compose above `IBackend`. In the build, `HttpCache` is only ever constructed
  inside `FFmpegBackend`'s open path — no other component builds one.
- `audio_extract.h` says ASR and the subtitle pipeline consume the extracted
  WAV. No such caller exists; only `tools/extract_audio` does, and the
  real-time ASR item in the roadmap is still unlanded.
- `CMakeLists.txt` still carries a comment that `soar_cast` is "not linked into
  the app yet". It is linked and both cast entry points are live.

Two comments in the cast sources are also stale in the opposite direction: they
say the session polls because eventing is "deliberately not done", and that
there is "one M-SEARCH burst". GENA subscription and renewal are implemented,
and discovery sends three bursts 300 ms apart.
