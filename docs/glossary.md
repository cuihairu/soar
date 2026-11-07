# Glossary

This table names the abstractions that survive file moves and build-target
changes. Paths are repository-relative and point at the owning declaration or
contract, not every call site.

See [Architecture overview](/architecture) for lifecycle and call channels,
and [Layering rationale](/rationale) for why each boundary exists.
Chinese version: [术语表](/zh/glossary).

| Term | English | Definition | Evidence |
| --- | --- | --- | --- |
| 入口应用 | Application entry | Shared console/GUI entry that parses flags, selects a backend, wires P2P/cast resources, and dispatches to headless or window execution. | `src/app/main.cpp:190`, `src/app/main.cpp:600` |
| 窗口界面 | Window UI | SDL2 window plus the Dear ImGui HUD, overlays, presentation path, and app-level input routing. | `src/app/ui/player_window.cpp:5099`, `src/app/ui/player_window.cpp:727` |
| 窗口配置 | Window UI config | Caller-owned runtime wiring between CLI resources and the window: paths, stores, atomic progress knobs, backend and bridge pointers. | `src/app/ui/player_window.h:27` |
| 主题令牌 | Theme tokens | Named colors, radii, fonts, and runtime palette state consumed by UI drawing. | `src/app/ui/player_window.cpp:336`, `src/app/ui/ui_tokens.h:17` |
| 界面纯状态 | UI state | Stateless-ish window logic: OSC focus, playlist, recents, directory scanning, and presentation-neutral navigation. | `src/app/ui/ui_state.h:203`, `src/app/ui/ui_state.h:219` |
| 最近记录 | Recent store | Recent-file/recent-directory MRU persisted as one URI per line with an atomic temporary-file rename. | `src/app/ui/ui_state.h:203`, `src/app/ui/ui_state.cpp:173` |
| 媒体库 | Media library | Watched-folder entries, playback progress, resume metadata, and optional metadata enrichment. | `src/app/ui/library.h:132`, `src/app/ui/library.cpp:295` |
| 快捷键表 | Shortcut map | Named actions bound to keys/modifiers, with defaults, persistent overrides, conflict stealing, and kill switch. | `src/app/ui/shortcuts.h:146`, `src/app/ui/shortcuts.cpp:157` |
| 缩略图服务 | Thumbnail service | Background reopen-and-grab worker with request queue/LRU frame cache so hover thumbnails avoid decode on the UI thread. | `src/app/ui/thumbnail_service.h:45` |
| 播放门面 | Player facade | Backend-agnostic operations and a single event callback; owns no decode thread. | `include/soar/core/player.h:11` |
| 播放契约 | Playback backend contract | Virtual open/control/query/interface and ASS/external-subtitle extension contract implemented by all backends. | `include/soar/core/backend.h:108` |
| 媒体源 | Media source | URI plus cache directory and hardware-decoder request; the only source descriptor handed to a backend. | `include/soar/core/backend.h:13` |
| 播放事件 | Playback event | State, metadata, position, buffering, download-progress, and error notifications crossing from a backend to its sink. | `include/soar/core/backend.h:73` |
| FFmpeg 后端 | FFmpeg backend | Concrete decode/audio/HDR/HWAccel/subtitle/screenshot implementation owning decode and audio threads. | `include/soar/core/ffmpeg_backend.h:70`, `src/core/ffmpeg_backend.cpp:3005` |
| 空后端 | Null backend | Thread-less scripted state machine for no-FFmpeg builds and tests. | `src/core/null_backend.cpp:12` |
| HTTP 缓存 | HTTP cache | One-URL sparse disk cache exposed to FFmpeg through `AVIOContext`; 256 KiB blocks tracked by a bitmap, HTTP-only. | `include/soar/core/http_cache.h:1`, `src/core/ffmpeg_backend.cpp:2576` |
| 侧车字幕提供方 | Sidecar subtitle provider | Finds and reads subtitle files adjacent to local media. | `include/soar/core/subtitle_provider.h:60` |
| HTTP 字幕提供方 | HTTP subtitle provider | Configurable http-only remote search/fetch client that degrades to no candidates when unset. | `include/soar/core/subtitle_provider.h:112`, `include/soar/core/subtitle_provider.h:88` |
| 字幕翻译器 | Subtitle translator | Optional OpenAI-compatible chat-completions client preserving cue count/timing while translating text. | `include/soar/core/subtitle_provider.h:207`, `include/soar/core/subtitle_provider.h:193` |
| 元数据抓取器 | Metadata scraper | Optional TMDB-shaped search client enriching library rows; unconfigured means no network. | `include/soar/core/metadata_scraper.h:56`, `include/soar/core/metadata_scraper.h:41` |
| ASS 渲染器 | ASS renderer | libass wrapper for fonts, external/streamed documents, canvas mapping, change detection, and PTS rendering. | `include/soar/core/ass_renderer.h:48` |
| 外部字幕存档 | External subtitle store | Sanitized temporary-file location where fetched remote subtitles become loadable local files. | `src/core/subtitle_provider.cpp:609` |
| 种子流桥 | Torrent stream bridge | libtorrent-backed download engine republished as a byte-range HTTP URL, with sync/async metadata lifecycle and LAN rebind. | `src/p2p/torrent_stream.h:53`, `src/p2p/torrent_stream.cpp:696` |
| 投屏会话 | Cast session | DLNA/UPnP AVTransport control-point orchestration: discovery, description parsing, bridge exposure, control, polling, teardown. | `src/cast/cast_session.h:39`, `src/cast/cast_session.cpp:80` |
| 渲染器客户端 | Renderer client | HTTP transport for SSDP discovery and SOAP AVTransport commands/state reads. | `src/cast/renderer.h:27`, `src/cast/renderer.h:47` |
| 设备描述 | Device description | Tolerant parser/resolver for DLNA device XML, locating AVTransport control and event endpoints. | `src/cast/device_xml.h:19`, `src/cast/device_xml.h:37` |
| SOAP 契约 | SOAP contract | Request builders and fault/position parsers for AVTransport operations. | `src/cast/soap.h:37`, `src/cast/soap.h:54` |
| GENA 事件 | GENA events | Subscription/renewal/unsubscription request machinery and NOTIFY state. | `src/cast/eventing.h:1`, `src/cast/eventing.cpp:44` |
| 事件服务 | Event server | Local TCP endpoint receiving bounded renderer NOTIFY callbacks and replying with bare success. | `src/cast/event_server.h:16`, `src/cast/event_server.cpp:121` |
| 转码服务 | Transcode server | Sender-side FFmpeg pipeline and local MPEG-TS HTTP server for receivers that cannot decode the source. | `src/cast/transcode_server.h:37`, `src/cast/transcode_server.cpp:728` |
