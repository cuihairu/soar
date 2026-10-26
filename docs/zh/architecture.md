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

## 图与代码不一致之处

本图以代码为准。旧文档里有三处与之不符，这里按代码写法描述：

- `mvp.md` 把缓存与下载层描述为可组合在 `IBackend` 之上的独立组件。实际上 `HttpCache` 只在 `FFmpegBackend` 的打开路径内部被构造，没有其他组件创建它。
- `audio_extract.h` 称 ASR 与字幕管线会消费提取出的 WAV。这样的调用方并不存在，只有 `tools/extract_audio` 在用，而路线图里的实时 ASR 仍未落地。
- `CMakeLists.txt` 仍留有一条「`soar_cast` 尚未链接进应用」的注释。它已链接，且两个投屏入口都是活跃的。

投屏源码里有两处注释则反向过期：它们写着因为「刻意不做事件」所以会话只能轮询，以及「一次 M-SEARCH 突发」。GENA 的订阅与续订都已实现，发现阶段实际发出的是三次间隔 300 毫秒的突发。
