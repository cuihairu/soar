# MVP 能力清单（建议）与边界

目标：先做一个“可用的桌面播放器内核 + 最薄 UI”，再逐步扩展到移动端与高级功能。

> 约定：本文只讨论功能边界与接口落点；不包含 DRM/专利/商店条款等合规细节（见 `docs/licensing.md`）。

## 0. 总体原则

- **先桌面后移动**：Windows/macOS/Linux 先把播放链路跑通，移动端优先走系统框架或单独后端。
- **先播放后媒体库**：先保证“打开→播放→暂停→拖动→切换音轨/字幕”体验稳定，再做扫描/刮削/同步。
- **先无 DRM**：Widevine/FairPlay 等 DRM 不纳入“万能”范围；需要时走系统播放器或商业方案。

## 1. MVP（v0.1）必须具备

### 1.1 媒体输入

- 本地文件：`file://` 或直接路径
- 基础 URL：`http(s)://`（是否支持分段/自适应留给后端）
- 清晰的错误与状态：打开失败原因可见

### 1.2 播放控制

- 播放/暂停/停止
- Seek（拖动进度条）
- 倍速（0.5x/1.0x/1.5x/2.0x）
- 音量（0–100%）与静音

### 1.3 轨道与字幕

- 读取 `MediaInfo`：时长、是否可 seek、轨道列表
- 选择音轨/字幕轨
- 关闭字幕

### 1.4 事件与指标（可观测）

- 状态变化事件：opening/playing/paused/stopped/ended/error
- 时间事件：position 更新（用于 UI 进度条）

### 1.5 桌面 UI（最薄一层）

- 浮动控制栏（OSC）：底部居中、2.5s 无输入自动隐藏，悬停/拖动/浮层打开/暂停时钉住
- 控件：整宽 seek 条（悬停时间提示、拖动预览、松手提交）+ 传输控制、时钟、音量、倍速、音轨/字幕菜单、信息、全屏
- 键鼠：单击切换 OSC、双击全屏、垂直滚轮音量 / 水平（或 Shift）滚轮 seek、拖放文件打开、最近打开
- 状态提示：暂停/缓冲指示、OSD Toast、媒体信息浮层

设计与技术选型（SDL2 自绘 / ImGui / Qt / Web 壳的对比与取舍）见 [ui-design.md](ui-design.md)。

## 2. v0.2（强烈建议尽快补齐）

- 播放列表：追加/删除/下一首/上一首/循环/随机
- 截图已落地：`S` 键把最近呈现的视频帧保存为 `screenshot.png`（FFmpeg 内置 PNG 编码器，YUV420P→RGB24 转换；失败只记 lastError 并 Toast 提示）
- 音频输出设备选择（桌面）；A-B 循环已落地：`L` 键三段（定 A → 定 B → 清除），解码线程到点回跳（到 EOF 也回跳，即尾锚定循环），手动 seek 解除（mpv 语义）
- 更完整协议：HLS/DASH/RTSP（取决于后端）
- 字幕体验：字体/大小/同步偏移（基础）
- 网络流基线：HTTP(S) 顺序播放冒烟、缓冲感知事件（见 §5 的 P0–P2）

## 3. v1.0（“万能播放器”更接近的形态）

- 硬件解码与 HDR 色彩链路（跨平台差异大）
- 投屏（AirPlay/Chromecast/DLNA）与远程控制
- 媒体库与刮削、历史/同步、多端一致性
- 插件体系（输入源/解码/字幕/渲染/扩展协议）
- 边下载边看：本地缓存下载 + 字幕下载 + AI 翻译（见 §5 的 P3–P4 与 §6）

## 4. 明确不做（或单独产品线）

- DRM（Widevine/FairPlay/PlayReady）通吃
- 绕过平台/商店限制的分发方式

## 5. 边下载边看（网络播放路线）

目标形态参考"迅雷看看"：给一个网络源就能立刻开播，播放过程中数据在后台落盘，seek 与回看都走本地缓存，断网/断流可恢复。FFmpeg 后端用 `avformat_open_input(url)` 直接吃 URL，HTTP(S) 顺序播放本质上已经是边下边看（socket 流式读）；真正的差距按阶段补齐：

| 阶段 | 内容 | 量级 |
|---|---|---|
| **P0 基线冒烟** | 验证现状：本地 HTTP 服务 + `--backend=ffmpeg http://...` 顺序播放、Range seek、断流行为；修发现的坑 | 小 |
| **P1 缓冲感知** ✅ | 核心事件模型加 `BufferingStarted`/`BufferingEnded` 事件（不动状态机）；AVIO 中断回调作网络卡顿看门狗：读停顿超 10s 上报缓冲、超 60s 容忍才中止进 Error（不主动断连，慢源在同一连接上自愈）；慢速 HTTP fixture 用例实证 | ✅ 完成 |
| **P2 自适应协议** ✅ | FFmpeg demuxer 原生支持 HLS/DASH/RTSP,后端零改动直通（不设 protocol_whitelist，默认全允许）；fixture：AAC 直出的单 variant/master 双 variant/MPD/内嵌 python RTSP-RTP 服务器（PCMA over UDP）；用例：HTTP 冒烟 ×3 + Range-capable server 的 VOD seek（seekable=true、Stopped 同步 seek、播放推进）+ RTSP 直播流冒烟（顺带修复：直播流负 duration 归 0、无 duration 的流拒绝 seek 防止 clamp UB） | ✅ 完成 |
| **P3 边下边存** | 自定义 AVIO 层：下载落盘本地缓存、播放从缓存读、seek 到未下载段发 Range 请求、断点续播、UI 下载进度。**P3a 最小内核 ✅**:独立 `HttpCache` 组件(双文件 `<fnv1a64(url)>.meta/.data`,位图 1 bit/256KiB 块、meta 记明文 url 不匹配即重建、data 稀疏 ftruncate;内置最小 http-only 客户端,SO_RCVTIMEO 60s 兜底)+ FFmpeg avio 薄适配(seek 回调含 AVSEEK_SIZE;`AVFMT_FLAG_CUSTOM_IO`)+ CLI `--cache-dir=`;同一 URL 离线可重播(集成用例:播完杀 server 重开推进);https 本批直通不走缓存。**P3b seek 补洞/断点续传 ✅**:集成用例钉死三契约——stopped 态 seek 进未缓存区只补目标块(位图断言头部~目标之间仍为洞、`cachedBytes<size`)、杀 server 后部分缓存离线重开在已缓存区间续播且位图零增长、离线播放走进洞报 Error 事件而非挂死(avio miss→EIO→fatal)。**P3c 下载进度事件 ✅**:缓存 avio 读回调在播放期按源大小 1/16 步进节流发 `DownloadProgress`(首发读只标定,离线全缓存零事件;payload 走 `Event::message` 的 `"downloaded/total"` 格式,Event 成员冻结政策见 coverage-notes §3.8);CLI 打印 + 窗口 Downloading 徽标,Xvfb 窗口化用例断言事件序列单调、以 100% 终结。批内真产品修复:avio 读回调源尾返回 0 被部分 FFmpeg 版本当"暂无数据"无限重试、缓存回放永远到不了 Ended——改为契约要求的 `AVERROR_EOF`。 | 中高 |
| **P4 P2P** | libtorrent（BSD，合规无冲突）做 piece 顺序优先下载；或本地 HTTP 代理桥接（BT 流伪装成 `http://localhost:port/`，FFmpeg 后端零改动） | 高 |

架构约定：**核心抽象不动**——网络能力是后端实现细节，唯一的核心层变更是 P1 的事件模型扩展；P3 起的缓存/下载层做成独立组件，`IBackend` 之上可组合，不绑死 FFmpeg。

## 6. 字幕生态（下载与 AI 翻译）

依赖顺序：先落地 v0.2 的字幕渲染基础（字体/大小/偏移），再接外部字幕源。

- **字幕下载**：按媒体哈希/文件名从 OpenSubtitles 等公开源检索、下载、与轨道对齐；做成可插拔的 `SubtitleProvider` 接口（核心定义接口，实现可换源），失败静默降级为无字幕。
- **字幕文本翻译**：已有字幕轨/字幕文件时，把文本批量送 LLM/翻译 API，生成翻译字幕轨（轻量路径）。
- **语音实时翻译**：无字幕轨媒体走 ASR（本地 whisper 类模型或云端）+ 翻译 + 字幕渲染（重量级路径，放最后）。

架构约定：翻译/ASR 服务走 **OpenAI 兼容端点由用户自配**（base URL + key），核心不内置任何云厂商凭据，不做强制联网；所有 AI 能力都是可选插件式服务，离线场景一切照旧。

