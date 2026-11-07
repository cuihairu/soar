# 拆分理由

分层图是一项抽象练习；本页记录为什么值得命名这些边界。

术语定义见[术语表](/zh/glossary)，状态与通道见[架构总览](/zh/architecture)。英文版：[Layering rationale](/rationale)。

1. **入口/UI 与播放分离，因为 UI 已经可以在没有真实解码器时编译运行。** 没有 ImGui 时 `runPlayerWindow` 仍存在并能呈现裸回退路径（`src/app/ui/player_window.cpp:5099`、`player_window.cpp:5205`），`NullBackend` 也提供无线程的同一 `Player` 契约（`src/core/null_backend.cpp:12`）。把动作留在 `Player` 后面因此有可执行的收益，而不只是风格收益。
2. **`Player` 的存在是因为 UI 不应学习后端生命周期或线程拓扑。** 门面刻意禁止拷贝/移动，且只拥有一个后端加回调（`include/soar/core/player.h:16`、`player.h:59`），FFmpeg 所有权隐藏在 `IBackend` 后（`include/soar/core/backend.h:108`）。后端把 ASS 暴露为可选借用能力，并在缺少 libass 时给出显式空契约（`backend.h:142`）。
3. **`HttpCache` 单独存在，因为它与播放的唯一集成点是自定义 `AVIOContext`，而其文件格式不依赖 FFmpeg。** 缓存契约明确声明它不了解 FFmpeg（`include/soar/core/http_cache.h:6`）；唯一构造点在 `src/core/ffmpeg_backend.cpp:2576` 把它接入后端。因此无需实例化解码路径即可诊断或回放缓存文件。
4. **字幕发现拆成三个提供方，因为它们的失败语义和线上模式不同，而播放必须容忍所有失败。** 共享契约要求不可达或畸形来源变成“无候选”，而不是播放失败（`include/soar/core/subtitle_provider.h:29`）；侧车规则依赖本地目录（`subtitle_provider.h:60`），HTTP 客户端有仅 HTTP 的搜索/下载协议（`subtitle_provider.h:88`），翻译有批量 OpenAI 兼容响应契约（`subtitle_provider.h:193`）。
5. **种子桥单独存在，因为 libtorrent 可以停在字节范围 HTTP URL 边界。** 桥契约明确让 `IBackend` 和 FFmpeg 不了解 torrent（`src/p2p/torrent_stream.h:1`）；播放只看到 `http://127.0.0.1:<port>/`（`torrent_stream.cpp:601`）。投屏是唯一被许可的例外，表现为临时监听器重绑定和 URL 重写（`torrent_stream.cpp:763`、`torrent_stream.cpp:784`）。
6. **投屏是独立控制点，因为远端对等端说的是 UPnP，不是进程内播放契约。** `CastSession` 的启动序列组合发现、XML、桥暴露、SOAP、可选 GENA 和轮询（`src/cast/cast_session.cpp:80`–`cast_session.cpp:150`）；每个协议都有独立解析器/构造器（`src/cast/ssdp.cpp:10`、`src/cast/device_xml.h:37`、`src/cast/soap.h:37`、`src/cast/eventing.cpp:44`）。媒体路径仍是桥的 HTTP URL，因此播放核心保持不变。
7. **P2P 与投屏状态独立于 UI 状态，因为它们是资源生命周期，不是偏好生命周期。** 入口先声明桥、后声明 Player，使播放先析构（`src/app/main.cpp:353`）；再在其后声明投屏，使会话先停止并恢复桥（`main.cpp:473`）。HUD 析构重复“先投屏、后存储”的顺序（`src/app/ui/player_window.cpp:879`）。偏好则是独立的尽力而为文本存储（`ui_state.cpp:173`、`library.cpp:540`、`shortcuts.cpp:448`）。
