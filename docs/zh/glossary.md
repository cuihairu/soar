# 术语表

下表收录在文件移动与构建目标变化后仍然成立的抽象。路径均相对仓库根目录，指向所有权声明或契约位置，而不是穷举调用点。

生命周期与调用通道见[架构总览](/zh/architecture)，边界成因见[分层理由](/zh/rationale)。英文版：[Glossary](/glossary)。

| 术语 | 英文 | 定义 | 证据 |
| --- | --- | --- | --- |
| 入口应用 | Application entry | 控制台/GUI 共用入口：解析参数、选择后端、装配 P2P 与投屏资源，并分流到无头或窗口执行。 | `src/app/main.cpp:190`, `src/app/main.cpp:600` |
| 窗口界面 | Window UI | SDL2 窗口、Dear ImGui HUD、浮层、送显路径和应用级输入路由。 | `src/app/ui/player_window.cpp:1019`, `src/app/ui/player_window.cpp:278` |
| 窗口配置 | Window UI config | 调用方拥有的运行时装配结构：路径、存储、原子进度开关，以及后端和桥指针。 | `src/app/ui/player_window.h:27` |
| 主题令牌 | Theme tokens | UI 绘制消费的命名颜色、圆角、字体和运行时调色板状态。 | `src/app/ui/player_hud.h:133`, `src/app/ui/ui_tokens.h:19` |
| 界面纯状态 | UI state | 与呈现解耦的窗口逻辑：OSC 焦点、播放队列、最近记录、目录扫描和中性行为。 | `src/app/ui/ui_state.h:203`, `src/app/ui/ui_state.h:219` |
| 最近记录 | Recent store | 按行持久化的最近文件/目录 MRU，使用临时文件加原子重命名。 | `src/app/ui/ui_state.h:203`, `src/app/ui/ui_state.cpp:173` |
| 媒体库 | Media library | 监视目录、播放进度、续播元数据和可选元数据补全。 | `src/app/ui/library.h:132`, `src/app/ui/library.cpp:295` |
| 快捷键表 | Shortcut map | 具名动作到按键/修饰键的绑定，含默认值、持久覆盖、冲突抢占和总开关。 | `src/app/ui/shortcuts.h:146`, `src/app/ui/shortcuts.cpp:157` |
| 缩略图服务 | Thumbnail service | 后台重新打开并取帧的工作线程，含请求队列和 LRU 帧缓存，避免 UI 线程解码。 | `src/app/ui/thumbnail_service.h:45` |
| 播放门面 | Player facade | 与后端无关的播放操作和唯一事件回调；不拥有解码线程。 | `include/soar/core/player.h:11` |
| 播放契约 | Playback backend contract | 所有后端实现的虚拟打开/控制/查询接口，以及 ASS 和外挂字幕扩展契约。 | `include/soar/core/backend.h:108` |
| 媒体源 | Media source | URI、缓存目录和硬件解码请求，是交给后端的唯一来源描述。 | `include/soar/core/backend.h:13` |
| 播放事件 | Playback event | 状态、媒体信息、位置、缓冲、下载进度和错误通知，从后端穿越到事件接收器。 | `include/soar/core/backend.h:73` |
| FFmpeg 后端 | FFmpeg backend | 解码、音频、HDR、硬件加速、字幕和截图的具体实现，拥有解码与音频线程。 | `include/soar/core/ffmpeg_backend.h:70`, `src/core/ffmpeg_backend_decode.cpp:15` |
| 空后端 | Null backend | 无线程的脚本化状态机，供无 FFmpeg 构建和测试使用。 | `src/core/null_backend.cpp:12` |
| HTTP 缓存 | HTTP cache | 通过 `AVIOContext` 暴露给 FFmpeg 的单 URL 稀疏磁盘缓存；256 KiB 块加位图，仅支持 HTTP。 | `include/soar/core/http_cache.h:1`, `src/core/ffmpeg_backend_open.cpp:161` |
| 侧车字幕提供方 | Sidecar subtitle provider | 发现并读取本地媒体旁路字幕文件。 | `include/soar/core/subtitle_provider.h:60` |
| HTTP 字幕提供方 | HTTP subtitle provider | 可配置且仅支持 HTTP 的远程搜索/下载客户端；未配置时退化为无候选。 | `include/soar/core/subtitle_provider.h:112`, `include/soar/core/subtitle_provider.h:88` |
| 字幕翻译器 | Subtitle translator | 可选的 OpenAI 兼容 chat-completions 客户端，翻译文本并保持 cue 数量与时序。 | `include/soar/core/subtitle_provider.h:207`, `include/soar/core/subtitle_provider.h:193` |
| 元数据抓取器 | Metadata scraper | 可选的 TMDB 形状搜索客户端，用于补全媒体库；未配置即不访问网络。 | `include/soar/core/metadata_scraper.h:56`, `include/soar/core/metadata_scraper.h:41` |
| ASS 渲染器 | ASS renderer | libass 封装：注册字体、装载外置/流式文档、映射画布、检测变化并按 PTS 渲染。 | `include/soar/core/ass_renderer.h:48` |
| 外部字幕存档 | External subtitle store | 远程字幕清洗后写入的临时文件位置，使其成为可加载的本地文件。 | `src/core/subtitle_provider.cpp:424` |
| 种子流桥 | Torrent stream bridge | 基于 libtorrent 的下载引擎，重新发布为字节范围 HTTP URL；含同步/异步元数据生命周期和 LAN 重绑定。 | `src/p2p/torrent_stream.h:53`, `src/p2p/torrent_stream.cpp:696` |
| 投屏会话 | Cast session | DLNA/UPnP AVTransport 控制点编排：发现、描述解析、桥暴露、控制、轮询和拆除。 | `src/cast/cast_session.h:39`, `src/cast/cast_session.cpp:80` |
| 渲染器客户端 | Renderer client | SSDP 发现与 SOAP AVTransport 命令/状态读取的 HTTP 传输层。 | `src/cast/renderer.h:27`, `src/cast/renderer.h:47` |
| 设备描述 | Device description | 容错解析并解析 DLNA 设备 XML，定位 AVTransport 控制与事件端点。 | `src/cast/device_xml.h:19`, `src/cast/device_xml.h:37` |
| SOAP 契约 | SOAP contract | AVTransport 操作的请求构造器，以及错误和位置响应解析器。 | `src/cast/soap.h:37`, `src/cast/soap.h:54` |
| GENA 事件 | GENA events | 订阅、续订、退订请求机制和 NOTIFY 状态。 | `src/cast/eventing.h:1`, `src/cast/eventing.cpp:44` |
| 事件服务 | Event server | 接收受限 NOTIFY 回调并返回裸成功的本地 TCP 端点。 | `src/cast/event_server.h:16`, `src/cast/event_server.cpp:121` |
| 转码服务 | Transcode server | 发送端 FFmpeg 管线与本地 MPEG-TS HTTP 服务，服务无法解码原始流的接收端。 | `src/cast/transcode_server.h:37`, `src/cast/transcode_server.cpp:728` |
