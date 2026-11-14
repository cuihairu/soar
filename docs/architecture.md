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

## Layer map

The five layers below are responsibilities, not directory names. Read the
state stores and channels as the boundaries where ownership changes.

### L1 — process entry and window UI

`src/app/main.cpp:190` parses the CLI schema and `src/app/main.cpp:600`
assembles `WindowUiConfig`; the runtime schema itself is declared at
`src/app/ui/player_window.h:27`. `runPlayerWindow` owns SDL2/ImGui startup
and teardown at `src/app/ui/player_window.cpp:1019`; `PlayerHud` is
constructed at `player_window.cpp:278` and owns application-level routing
through `handleEvent` at `player_window.cpp:504` — the HUD itself is split
across `player_hud_*.cpp`, one file per concern. Theme and font literals
live in the `theme` namespace at `src/app/ui/player_hud.h:133` (geometry
tokens in `src/app/ui/ui_tokens.h:19`); action labels and help text are
drawn in `PlayerHud`, and shortcut names/defaults live beside their parser
in `src/app/ui/shortcuts.cpp:111` and `src/app/ui/shortcuts.cpp:157`.
Window preference files are `theme.txt` (`player_hud.h:311`) and
`settings.txt` (`player_hud.h:348`); path construction and startup output
use `SOAR_UI_BITMAP_FONT`, `SOAR_STARTUP_LOG`, and `SOAR_NO_DIALOG`
(`player_hud.h:478`, `src/app/startup_report.cpp:26`, and
`src/app/startup_report.cpp:60`).

This layer's files are the recent lists (`ui_state.cpp:158`), media library
(`library.cpp:540`), and shortcut map (`shortcuts.cpp:448`). It calls the
Player facade by C++ method; backend and bridge objects are passed through
the config as opaque pointers (`player_window.h:16`, `player_window.h:63`).

### L2 — playback core

`Player` is the narrow facade (`include/soar/core/player.h:11`);
`IBackend` is the ownership and capability boundary
(`include/soar/core/backend.h:108`). `MediaSource` is the complete open
request (`backend.h:13`), and `Event` is the callback vocabulary
(`backend.h:73`). The schema for a concrete backend's internal configuration
is its private state, declared in `include/soar/core/ffmpeg_backend.h:70`;
`decodeLoop` is the thread boundary at
`src/core/ffmpeg_backend_decode.cpp:15`, spawned from
`src/core/ffmpeg_backend.cpp:417`.
`src/core/null_backend.cpp:12` supplies the no-FFmpeg state machine. There
is no separate protocol file: this layer's "protocol" is the virtual ABI in
`backend.h`.

### L3 — media services

`HttpCache` owns the `.meta`/`.data` file pair and HTTP Range client
(`include/soar/core/http_cache.h:37`; filenames at
`src/core/http_cache.cpp:545`). It is deliberately injected only by
`FFmpegBackend` through `AVIOContext`
(`src/core/ffmpeg_backend_open.cpp:161`).
`SidecarSubtitleProvider`, `ExternalSubtitleProvider`, and
`SubtitleTranslator` share the provider abstraction
(`include/soar/core/subtitle_provider.h:37`) but own separate wire schemas:
sidecar/local layout at `subtitle_provider.h:60`, HTTP search/fetch at
`subtitle_provider.h:88`, and chat-completions translation at
`subtitle_provider.h:193`. The metadata API has the same configuration and
wire contract in comments (`include/soar/core/metadata_scraper.h:35`).
`AssRenderer` is the single libass boundary (`ass_renderer.h:48`), and
`storeExternalSubtitle` turns remote payloads into sanitized local files
(`src/core/subtitle_provider.cpp:424`). The UI reads endpoint/key/timeout
from `SOAR_SUBTITLE_*`, `SOAR_TRANSLATE_*`, and `SOAR_META_*`
(`player_window.cpp:335`, `player_window.cpp:354`,
`player_window.cpp:378`).

### L4 — P2P bridge

`TorrentStream` accepts a `.torrent`/magnet and params in
`src/p2p/torrent_stream.h:62`, then publishes either a synchronous or async
`Connecting→Serving` lifecycle (`torrent_stream.cpp:696`,
`torrent_stream.cpp:709`). Its only interface to playback is the loopback
byte-range HTTP URL (`torrent_stream.cpp:601`); casting can temporarily
rebind that listener to the LAN (`torrent_stream.cpp:763` and
`torrent_stream.cpp:784`). Pieces are real files under `store_dir`; main
selects `<tmp>/soar-torrent` by default (`src/app/main.cpp:376`) and wires
store/index/peers at `src/app/main.cpp:389`. CLI flags and the file-table
output are owned in the entry schema (`main.cpp:213`,
`main.cpp:408`). No player or UI object learns torrent semantics.

### L5 — casting control point

`CastSession` orchestrates the DLNA workflow and state machine
(`src/cast/cast_session.h:39`); its start order—discover, parse, expose
bridge, optionally transcode, SOAP commands, optional GENA, poll—is visible
from `src/cast/cast_session.cpp:40` through `cast_session.cpp:150`. The
wire protocols are separated by file: SSDP (`src/cast/ssdp.cpp:10`),
device XML (`src/cast/device_xml.h:37`), SOAP
(`src/cast/soap.h:37`), and GENA (`src/cast/eventing.cpp:44`). The local
callback and media endpoints are `EventServer`
(`src/cast/event_server.h:16`) and `TranscodeServer`
(`src/cast/transcode_server.h:37`). UI cast state, discovery, and per-frame
auto-cast remain in `PlayerHud`: discovery and the per-frame send sit in
`player_hud_actions.cpp:502` and `:520`, the overlay in
`player_hud_actions.cpp:590`, and headless casting is assembled in
`main.cpp:566`. User-visible cast feedback comes from `PlayerHud` toasts;
no centralized copy file exists.

### State and channels

| Component | State | Channel |
| --- | --- | --- |
| Window UI | Text stores in the platform state directory plus in-memory ImGui state | Direct calls to `Player`; atomic pointers from config |
| Player/FFmpegBackend | Decode/audio frame queues, decoder/session objects, SDL audio device | Virtual `IBackend`; backend→UI `Event` callback |
| HTTP cache | Hash-named meta/data files | Custom FFmpeg `AVIOContext`; blocking HTTP Range |
| Subtitles/metadata | Temporary subtitle files; optional remote endpoints | Blocking HTTP clients; direct provider calls |
| Torrent bridge | Real torrent files in `store_dir`; socket/thread state | Loopback or LAN HTTP byte-range server |
| Cast layer | Ephemeral sockets and session state | SSDP UDP, HTTP XML/SOAP/GENA, local HTTP TS/NOTIFY servers |

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
