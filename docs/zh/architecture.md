# 架构总览

Soar 是单进程应用：同一个二进制承载桌面界面、播放核心、媒体服务、P2P 桥与投屏控制点。进程之外的部分只有它对话的对等端——DLNA 接收端、种子群、字节范围 HTTP 源，以及可选的 API 端点。

<picture>
  <source media="(max-width: 640px)" srcset="/architecture/system-zh-mobile.svg">
  <img src="/architecture/system-zh.svg" alt="Soar 播放进程与网络对等端：入口、播放核心、媒体服务、P2P 桥、投屏控制点，以及它们对话的外部对等端" width="100%">
</picture>

线型含义：实线表示媒体、控制指令与结果流；虚线表示发现与订阅流量（SSDP 发现、GENA NOTIFY）。

## 进程

`soarAppMain` 是控制台构建（`soar`）与 Windows GUI 构建（`soarw.exe`）共用的入口：解析参数、选择后端，然后移交。`player_window` 持有 SDL2 窗口、ImGui 表面、屏幕控制、浮层与送显路径。`ui_state` 是围绕它的纯逻辑——OSC 自动隐藏、最近文件、目录列表、按键映射；`library` 则把监视目录与续播位置保存在 `library.txt` 中。

## 播放核心

`Player` 是带单一事件回调的薄门面，不持有任何线程。它委派给 `IBackend`，后者有两个实现：`FFmpegBackend` 是唯一的 FFmpeg 构建，持有解码线程、硬件解码、HDR→SDR 色调映射、音频输出与截图；`NullBackend` 是无线程的测试替身，供无 FFmpeg 构建与测试套件使用。两者通过同一套契约送帧，UI 从不感知自己对接的是哪个后端。

## 媒体服务

`HttpCache` 是一个自定义的 FFmpeg `AVIOContext`：256 KiB 块由逐块一位的位图跟踪，仅在 `http://` URL 上启用、且只由 `FFmpegBackend` 构造，没有淘汰策略。`Subtitles + Ass` 覆盖侧车与目录两种字幕提供方、容错解析器、可选翻译器，以及 `AssRenderer`——唯一的 libass 调用方，其画布由 UI 合成到视频之上。`TorrentStream` 仅把 libtorrent 当作下载内核，再通过 `127.0.0.1` 的字节范围 HTTP 桥对外提供已下载的文件，因此后端只是打开一个普通 URL，对 P2P 一无所知。

## 投屏

`CastSession` 是一个 DLNA 控制点：通过 SSDP 发现接收端，用 SOAP 走 AVTransport，并订阅 GENA 事件。它的周边还有两个本地 HTTP 服务器——事件服务器承接接收端的 NOTIFY 回调，转码服务器则向接收端输出 MPEG-TS 流。虚线即这条订阅及其 NOTIFY 回执；实线则是控制流量与流出的媒体。

## 磁盘与可选端点

本地磁盘存放媒体文件、缓存块与 `library.txt`。字幕目录、元数据检索与翻译 API 都是通过环境变量注入的可选端点；一个都不配置时播放器照常工作，只是少了这些能力。

## 分层图

下面五层是职责，不是目录名。应把状态存储和调用通道理解为所有权发生变化的边界。

### L1 —— 进程入口与窗口界面

`src/app/main.cpp:190` 解析 CLI 模式，`src/app/main.cpp:600` 组装 `WindowUiConfig`；运行时模式声明在 `src/app/ui/player_window.h:27`。`runPlayerWindow` 在 `src/app/ui/player_window.cpp:5099` 持有 SDL2/ImGui 的启动和拆除；`src/app/ui/player_window.cpp:727` 的 `PlayerHud` 持有应用级路由。主题与字体字面量位于 `src/app/ui/player_window.cpp:336` 的运行时令牌表；动作标签和帮助文案由 `PlayerHud` 绘制，快捷键名称与默认值同解析器放在一起（`src/app/ui/shortcuts.cpp:111`、`src/app/ui/shortcuts.cpp:157`）。窗口偏好文件是 `theme.txt`（`player_window.cpp:487`）和 `settings.txt`（`player_window.cpp:525`）；路径构造与启动输出使用 `SOAR_UI_BITMAP_FONT`、`SOAR_STARTUP_LOG`、`SOAR_NO_DIALOG`（`player_window.cpp:651`、`src/app/startup_report.cpp:26`、`src/app/startup_report.cpp:60`）。

本层的文件是最近列表（`ui_state.cpp:158`）、媒体库（`library.cpp:540`）和快捷键表（`shortcuts.cpp:448`）。它用 C++ 方法调用 Player 门面；后端和桥对象作为不透明指针穿过配置（`player_window.h:16`、`player_window.h:63`）。

### L2 —— 播放核心

`Player` 是窄门面（`include/soar/core/player.h:11`）；`IBackend` 是所有权与能力边界（`include/soar/core/backend.h:108`）。`MediaSource` 是完整打开请求（`backend.h:13`），`Event` 是回调词汇表（`backend.h:73`）。具体后端的内部配置模式就是其私有状态，声明在 `include/soar/core/ffmpeg_backend.h:70`；`decodeLoop` 是 `src/core/ffmpeg_backend.cpp:3005` 的线程边界。`src/core/null_backend.cpp:12` 提供无 FFmpeg 状态机。这里没有单独协议文件：本层的“协议”就是 `backend.h` 中的虚拟 ABI。

### L3 —— 媒体服务

`HttpCache` 拥有 `.meta`/`.data` 文件对和 HTTP Range 客户端（`include/soar/core/http_cache.h:37`；文件名在 `src/core/http_cache.cpp:545`）。它刻意只由 `FFmpegBackend` 通过 `AVIOContext` 注入（`ffmpeg_backend.cpp:2576`）。`SidecarSubtitleProvider`、`ExternalSubtitleProvider` 和 `SubtitleTranslator` 共享提供方抽象（`include/soar/core/subtitle_provider.h:37`），但各自拥有独立线上模式：本地侧车在 `subtitle_provider.h:60`，HTTP 搜索/下载在 `subtitle_provider.h:88`，chat-completions 翻译在 `subtitle_provider.h:193`。元数据 API 的配置与协议同样写在注释契约中（`include/soar/core/metadata_scraper.h:35`）。`AssRenderer` 是唯一 libass 边界（`ass_renderer.h:48`），`storeExternalSubtitle` 把远程载荷转成清洗后的本地文件（`src/core/subtitle_provider.cpp:609`）。UI 从 `SOAR_SUBTITLE_*`、`SOAR_TRANSLATE_*`、`SOAR_META_*` 读取端点/密钥/超时（`player_window.cpp:786`、`player_window.cpp:804`、`player_window.cpp:828`）。

### L4 —— P2P 桥

`TorrentStream` 在 `src/p2p/torrent_stream.h:62` 接受 `.torrent`/magnet 和参数，随后发布同步或异步的 `Connecting→Serving` 生命周期（`torrent_stream.cpp:696`、`torrent_stream.cpp:709`）。它与播放之间只有回环字节范围 HTTP URL（`torrent_stream.cpp:601`）；投屏可临时把监听器重绑到 LAN（`torrent_stream.cpp:763`、`torrent_stream.cpp:784`）。分片是 `store_dir` 下的真实文件；入口默认选择 `<tmp>/soar-torrent`（`src/app/main.cpp:376`），并在 `src/app/main.cpp:389` 装配目录/索引/peer。CLI 参数与文件表输出由入口模式拥有（`main.cpp:213`、`main.cpp:408`）。任何 Player 或 UI 对象都不学习种子语义。

### L5 —— 投屏控制点

`CastSession` 编排 DLNA 工作流和状态机（`src/cast/cast_session.h:39`）；从 `src/cast/cast_session.cpp:80` 到 `cast_session.cpp:150` 可见启动顺序——发现、解析、暴露桥、按需转码、SOAP 命令、可选 GENA、轮询。线上协议按文件拆分：SSDP（`src/cast/ssdp.cpp:10`）、设备 XML（`src/cast/device_xml.h:37`）、SOAP（`src/cast/soap.h:37`）、GENA（`src/cast/eventing.cpp:44`）。本地回调和媒体端点是 `EventServer`（`src/cast/event_server.h:16`）与 `TranscodeServer`（`src/cast/transcode_server.h:37`）。UI 投屏状态、发现和逐帧自动投屏仍在 `PlayerHud`（`player_window.cpp:2035`、`player_window.cpp:2115`），无头投屏在 `main.cpp:566` 装配。用户可见的投屏反馈来自 `PlayerHud` toast；没有集中文案文件。

### 状态与通道

| 组件 | 状态 | 通道 |
| --- | --- | --- |
| 窗口界面 | 平台状态目录中的文本存储，加上内存中的 ImGui 状态 | 直接调用 `Player`；配置中的原子指针 |
| Player/FFmpegBackend | 解码/音频帧队列、解码器和会话对象、SDL 音频设备 | 虚拟 `IBackend`；后端到 UI 的 `Event` 回调 |
| HTTP 缓存 | 哈希命名的 meta/data 文件 | 自定义 FFmpeg `AVIOContext`；阻塞式 HTTP Range |
| 字幕/元数据 | 临时字幕文件；可选远程端点 | 阻塞式 HTTP 客户端；直接提供方调用 |
| 种子桥 | `store_dir` 下的真实 torrent 文件；套接字/线程状态 | 回环或 LAN 的 HTTP 字节范围服务 |
| 投屏层 | 临时套接字和会话状态 | SSDP UDP、HTTP XML/SOAP/GENA，本地 HTTP TS/NOTIFY 服务 |

## 图与代码不一致之处

本图以代码为准。旧文档里有三处与之不符，这里按代码写法描述：

- `mvp.md` 把缓存与下载层描述为可组合在 `IBackend` 之上的独立组件。实际上 `HttpCache` 只在 `FFmpegBackend` 的打开路径内部被构造，没有其他组件创建它。
- `audio_extract.h` 称 ASR 与字幕管线会消费提取出的 WAV。这样的调用方并不存在，只有 `tools/extract_audio` 在用，而路线图里的实时 ASR 仍未落地。
- `CMakeLists.txt` 仍留有一条「`soar_cast` 尚未链接进应用」的注释。它已链接，且两个投屏入口都是活跃的。

投屏源码里有两处注释则反向过期：它们写着因为「刻意不做事件」所以会话只能轮询，以及「一次 M-SEARCH 突发」。GENA 的订阅与续订都已实现，发现阶段实际发出的是三次间隔 300 毫秒的突发。
