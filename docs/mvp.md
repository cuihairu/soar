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
- 加载外部字幕（sidecar）：`loadExternalSubtitle` 把媒体同目录的 `.srt`/`.vtt`/`.ass`/`.ssa` 挂成一条字幕轨并可选中（ASS/SSA 文档的渲染口径见 §6 批 1b），字幕菜单与 Subtitle Settings 浮层列出候选（见 §6）

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

- 播放列表已落地：`soar <path> [more-paths...]` 多位置参数入队（首个即播、其余排队，mpv 语义；headless 保持单源契约）；`N`/`Shift+N` 下一首/上一首（越界只 Toast 不绕回），自然播完自动步进（Loop::Off 走到队尾即停在 Ended，空格可重播）；`P` 浮层列出队列：点选条目即播、`x` 删除（删除当前项只把队列重指到相邻条目，不打断播放）、Loop 按钮 off→all→one 三态循环、Shuffle 随机；模型是 ui_state.h 里的 PlaylistStore（循环/随机/advance 语义有单测），窗口链路（入队/步进/自动接续/浮层）有 X11 注入用例以两夹具的 media-info 轨迹验证全流程
- 截图已落地：`S` 键把最近呈现的视频帧保存为 `screenshot.png`（FFmpeg 内置 PNG 编码器，YUV420P→RGB24 转换；失败只记 lastError 并 Toast 提示）
- 音频输出设备选择已落地（桌面）：`IBackend` 增加端点枚举/查询/切换三方法（空串=系统默认），FFmpeg 后端经 SDL2 枚举设备，解码线程在下一个音频帧按新端点重开（旧端点队列自然放完，无突兀间断；暂停时切换在恢复播放时生效），OSC 音轨菜单尾部列出「Output: default」+ 主机端点；选择是端点状态而非媒体状态，open 前即可用，close 不重置
- A-B 循环已落地：`L` 键三段（定 A → 定 B → 清除），解码线程到点回跳（到 EOF 也回跳，即尾锚定循环），手动 seek 解除（mpv 语义）
- 更完整协议已落地：HLS/DASH 走本地 HTTP server 冒烟（单 variant/master 双 variant VOD seek 与播放推进，`SOAR_TEST_HLS_*`/`SOAR_TEST_DASH_AUDIO` 夹具），RTSP 走测试内嵌的最小 RTSP 服务器（DESCRIBE/SETUP/PLAY + PCMA over RTP）做直播流冒烟；三者都是 FFmpeg demuxer 透传，后端零改动（已知边界：`.sdp` 文件入口会被 libavformat 对嵌套 rtp 子流的默认白名单拒绝，rtsp:// 直连不受影响）
- 字幕体验已落地（基础）：字幕设置浮层（字号滑条、同步偏移、可见性），`V` 显隐、`[`/`]` 偏移步进，渲染与解析管线见 §1.3 与字幕批次说明
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
| **P4 P2P** | libtorrent（BSD，合规无冲突）做 piece 顺序优先下载；或本地 HTTP 代理桥接（BT 流伪装成 `http://localhost:port/`，FFmpeg 后端零改动）。**决策（2026-10-01）：走本地 HTTP 代理桥接**——libtorrent 仅作下载内核（vendor 钉版本入库，见 `third_party/libtorrent/VENDOR.md`），`TorrentStream` 组件把选中文件以带 Range 的 `http://127.0.0.1:<port>/` 呈现，播放链路复用既有 http 入口、`IBackend`/FFmpeg 后端零改动，符合「核心抽象不动」架构约定；桥接组件落在 `src/p2p/`，暂不进覆盖率门禁（测试批收编）。**P4a 最小内核 ✅**:vendored libtorrent 2.0.15（prune + 一处 `add_subdirectory(bindings)` guard，仅 Boost 头依赖;`encryption=OFF` 走捆绑 SHA-1/256、OpenSSL 零依赖——真实 swarm 里强制 MSE 加密的 peer 连不上，P4b 边界）+ `TorrentStream`（`src/p2p/`，libtorrent 头不进公共接口）:127.0.0.1-only HTTP 服务（随机端口、Range 三形态 206/416、keep-alive、256KiB 分块供流）+ 顺序下载 + 按需取件（读前缺件 `set_piece_deadline` + 25ms 轮询，60s 超时断链由 FFmpeg 走普通读错误）;CLI `.torrent` 位置参数即播（`--torrent-store=`/`--torrent-peer=` host:port 可重复/`--torrent-index=`），magnet 显式拒绝留 P4b;走查（真实双会话 swarm，`tools/seed_torrent` 建种/限速）:26MB mp4 在 1.2MB/s 限速下播放与下载并行推进、下载中途 RIGHT 前跳与 HOME 回看（杀种后纯本地续播 7s+）、无种子重开同一 store 复检 12.2MiB 断点续播、ESC 干净退出;受影响最小测试面（cli/http_cache 套件）绿;ci.yml 覆盖率门禁暂以 `--exclude '^src/p2p/'` 排除（双向实证），测试批收编。**P4b-1 magnet ✅**:`magnet:?xt=urn:btih:` 位置参数即播——infohash 兼容 40 位十六进制与 32 位 base32（libtorrent `parse_magnet_uri`，仍不进公共接口），`tr=` tracker 进 announce 队列、`x.pe=` 内置 peer 直连，DHT 用 session 默认 bootstrap 节点，元数据（info dictionary）由 swarm 送达后走既有文件几何初始化与 HTTP 桥（`start()` 里统一等 `torrent_file()` 就绪，.torrent 路径该等待为空转，行为零回退）;拿不到元数据 60s 显式报错（`timed out waiting for metadata`，带 peers 计数），解析失败显式报错（missing/invalid info-hash 等 error_code 原文）;走查:三腿畸形 magnet 显式秒报错（exit 1）、hex magnet + `--torrent-peer` 限速 1.2MB/s 边下边看（RIGHT 前跳/HOME 回看/杀种 peers=0 续播后 ESC 干净退出）、base32 magnet 仅靠 `x.pe=` 起播（杀种点 34.4s 本地续到 39.4s 后停在前沿，按码率与已下载量自洽）、随机 infohash 无 peer 60.2s 准时报错、`.torrent` 位置参数回归绿。DHT 公网寻 peer 与真实 tracker announce 未在本地 swarm 验证（无 tracker/DHT 节点可用），机制上走 libtorrent 内建路径。**P4b-2 下载进度 UI ✅**:`TorrentStream::on_progress` 接进 main.cpp 既有下载徽标（P3c 的 `Downloading N%`，bytes==total 隐藏，复用零新 UI 资产）;元数据等待期 `start()` 内也以 ~2Hz 调 on_progress，终端 ticks 显 peers 发现进展。**架构约束如实记载**:magnet 元数据等待发生在 `start()` 同步阻塞期（桥无 Content-Length 无法先行开窗——窗口在 `player.open()` 之后才存在），故「获取元数据中」窗口徽标在当前阻塞式 open 下不可观测，未做死 UI；若未来 P4b 改异步 open（先开窗后后台 open），徽标接线即现成。走查:无种子期终端 2Hz ticks（5s 10 条 peers=0）、中途起 seeder 元数据到 → 开窗、`Downloading 45%` 截图与 stderr 读数吻合（11.3/24.9MiB）、100% 徽标隐藏、ESC 干净退出。**P4b-3 多文件种子选择 ✅**:`Params::on_files` 回调——元数据就绪后、文件选择应用前发射全文件表（index/path/size）;越界 `--torrent-index` 报错带文件表（bounded 32 条防病态种子刷屏）;CLI `--torrent-list` 打表即退（.torrent 即刻、magnet 先经一次元数据获取;复用 `start()` 全链路含 `--torrent-peer`,stdout 出表便于管道,成功 exit 0 不开播放器）;窗口源名——`WindowUiConfig::source_label`（桥 URL 的显示名替换为种子内文件名,海报/标题/toast/播放列表统一走 `displayName()`,空则回退 URI base name）,窗口标题非空时拼 `soar - <文件名>`;`seed_torrent --file=<目录>` 建多文件种子（root=目录 basename）。**走查揪出并修复多文件服务 bug**:`streamRange` 读盘把文件内偏移加上了 `file_base`（绝对 torrent 偏移）,单文件种子 file_base==0 掩盖之,index≥1 的读盘全部越过文件 EOF（curl 隔离复现:206 头正常、0 字节 body 即断）,修复为读盘用文件相对偏移、`file_base` 只用于 piece 映射;修复后走查:多文件 `--torrent-list` 打表 exit 0、坏 index 报错带表 exit 1、index 1 窗口播放（标题 `soar - bunny.mp4`、暂停 HUD、进度 01:02/01:30）、index 2 为 10 字节文本显式开失败（Invalid data,exit 1）、null 后端海报显示 `bunny.mp4`（非桥 URL）、单文件 `.torrent` 回归绿（24.9MiB 播完）、受影响最小测试面（cli/ui 套件）2/2 绿。 | 高 |

架构约定：**核心抽象不动**——网络能力是后端实现细节，唯一的核心层变更是 P1 的事件模型扩展；P3 起的缓存/下载层做成独立组件，`IBackend` 之上可组合，不绑死 FFmpeg。

## 6. 字幕生态（下载与 AI 翻译）

依赖顺序：先落地 v0.2 的字幕渲染基础（字体/大小/偏移），再接外部字幕源。

- **内嵌 ASS/SSA 样式渲染**：内嵌 ASS/SSA 轨不再降采样成纯文本——样式/字体/颜色/定位经 **libass**（FFmpeg 生态成熟的开源 ASS 渲染库，Linux 走 apt `libass-dev`、mac/win 走 vcpkg `libass` port）渲染。**依赖与自研的边界**：渲染本体全部是 libass 的能力；本仓库自研的是接线与合成——`AssRenderer` 薄封装组件（libass 头不进公共头；把 libass 的字形位图按 straight-alpha src-over 合成为一整张 RGBA 画布，`changed` 语义让纹理只在画面变化时重新上传；内部互斥让解码线程喂事件与 UI 线程渲染共享一个实例，`available()=false` 的无 libass 构建自动退回既有纯文本路径，与 SDL2/FFmpeg/ImGui 同一套可选能力降级口径）；FFmpeg 后端打开时注册 Matroska 附件字体（`ass_add_font`）、把 CodecPrivate（脚本头+样式）喂给流式轨，并把 libavcodec 的 ASS 载荷重组回 libass 要的完整 `Dialogue:` 行——libavcodec 的 ass 解码器给的是 8 逗号 `ff_ass_get_dialog` 形式（无 `Dialogue:` 前缀、无起止时间，时长在 Matroska BlockDuration / 包 duration 上），按「rect 字段 + 包 pts/duration」拼回 10 字段完整行是接线的关键一步；UI 在视频 letterbox 之后直接 blend 画布纹理（无 ImGui 的裸窗口路径同样生效）。字体解析固定 `ASS_FONTPROVIDER_NONE` + 容器附件字体：无 fontconfig、无系统字体枚举，渲染确定性可测——测试夹具用仓库内附的 Noto Mono（SIL OFL 1.1，`tests/fixtures/FONT-LICENSE.txt`）并逐像素断言「每个可见像素恰为脚本声明的纯红色」。seek 倒退时 `ass_flush_events` 清事件，防止解码器重扫造成的重复叠加。样式渲染跟随被选中的内嵌 ASS/SSA 轨——open 挑中的默认轨在打开时即喂入，切换到非默认 ASS 轨时按该轨自己的 CodecPrivate 重建流式轨（内嵌轨选择语义批，见下文已知边界段）；内嵌文本轨未被选中时仍走原纯文本队列（外挂 SRT/WebVTT 自批 1c 起在有 libass 的构建走画布，见下）。
- **外挂 .ass/.ssa 文档字幕 ✅（批 1b）**：外挂 ASS/SSA sidecar 成为一等字幕轨，与内嵌 ASS 共用同一个 `AssRenderer`（渲染能力同上条，仍是 libass 的；本批自研的是「渲染器跟随最后选中的 ASS 源」的重定向接线）。**加载链路**：sidecar 候选扩展 `.ass`/`.ssa`（枚举 + 大小写不敏感扩展名 + 内容探测 `[Script Info]`/`Dialogue:`，SRT/VTT cue 文本里引用 "Dialogue:" 不误判），HTTP 检索协议的扩展名字段同收 `ass`/`ssa`，`storeExternalSubtitle` 落 `.ass` 后缀；`loadExternalSubtitle` 对 ASS 文档先做 `assDocumentCues` 抽取作加载门（一条可用 Dialogue 都没有的文件不算字幕），有 libass 时整篇 `loadDocument`（样式忠实，无 libass 构建降级为纯文本轨——抽取即轨道，{\...} 覆盖块剥离、\N/\n→换行、\h→空格、end≤begin 给 2s 默认显示时长，容错口径与 SRT 解析一致）。**重定向语义**：选文档轨 → 关内嵌流式 feed、整篇加载（画布随后续播放头走）；选回内嵌 ASS 轨 → 用 open 时 stash 的 CodecPrivate 重建流式轨（事件从解码器当前位置续流，**无自动重扫**：本批不补的边界是选回点之前的 cue 要等下次倒退 seek 重扫才显示）；选内嵌文本轨或 `disableSubtitles` → 换入空轨清画布（外挂文本轨的选中自批 1c 起也走文档路径，见下条）。文档激活期解码线程丢弃内嵌流事件（防止画布上叠一层纯文本回声），两个方向旗标都是 atomic（UI 线程写、解码线程读，与 renderer 自带互斥分层）。**UI 字体**：外挂文档没有容器附件字体，runWindow 用与 UI 字体同一候选表给 renderer 设 default font（best-effort——没有可用字体文件时，字体可从注册字体/附件解析的文档照常渲染）。测试：`assDocumentCues` 单测（时间戳宽度、文本变换、坏行容忍、CRLF）、sidecar/HTTP 候选/存储扩展名用例，媒体套件三用例——外挂文档逐像素绿断言（两种构建各一条路径：libass 画布 vs 纯文本泵）、选回内嵌流恢复红断言、disable 释放画布断言。
- **字幕下载**：按媒体哈希/文件名从 OpenSubtitles 等公开源检索、下载、与轨道对齐；做成可插拔的 `SubtitleProvider` 接口（核心定义接口，实现可换源），失败静默降级为无字幕。**接口、本地 sidecar 实现与播放链路已落地**：`SubtitleProvider` 只有 `findCandidates`（列候选）与 `fetch`（取文本）两个方法，`SidecarSubtitleProvider` 在媒体同目录找同名 `.srt`/`.vtt`（支持 `movie.en.srt`、`movie.en.forced.srt` 这类语言/forced 标签，大小写不敏感），SRT 与 WebVTT 的文本解析是无依赖纯函数、容错口径写在头注释里（BOM、CRLF/裸 CR、缺小时字段、1-6 位小数、WebVTT 的 NOTE/STYLE/头部块、坏块跳过继续等）。`IBackend::loadExternalSubtitle(path, out_id)` 把一个 sidecar 挂成一条真正的字幕轨：外部轨 id 从容器流数起算（`nb_streams + slot`），永不与内嵌轨冲突，轨名是文件名、codec 按内容判为 `subrip`/`webvtt`；`selectTrack` 相应放宽到接受这个 id。sidecar 没有解码线程，所以帧由播放头在 `tryGetSubtitleFrame` 里泵进**内嵌字幕同一个队列**（UI 渲染零改动、也分不清两者来源）：播放头倒退（seek / A-B 回绕 / 重播）时游标从头重扫，前进跳过大段时只入队最后 4 条。加载失败（无媒体 / 读不了 / 无 cue）只落 `lastError`、不发 Error 事件——sidecar 缺失是常态，媒体照播。UI 侧在字幕菜单与 Subtitle Settings 浮层里列出候选（只在菜单打开时枚举目录，无文件对话框），选中即加载并选中。**检索/下载源已落地（可插拔 HTTP 客户端，自定义行协议，不宣称兼容 OpenSubtitles 等现有服务 API）**：`ExternalSubtitleProvider` 用一个自含的小型阻塞 HTTP 客户端对话一个**文档化的极简行协议**——检索 `GET {endpoint}?size={字节}&hash={16 位 hex}&name={主文件名}`（key 走 `X-API-Key` 头），200 应答每行一个候选、TAB 分隔 url/语言/标题/扩展名、`#` 为注释行，只收 `http://` 候选（TLS 与 http_cache 一样明确不在范围内）；下载 `GET {候选 url}`，应答体须能被核心解析器识别为 SubRip/WebVTT 才算成功（200 的 HTML 错误页不会变成字幕轨）。哈希取「头尾各 64 KiB 的 u64 小端词求和 + 文件大小」（`mediaHashHex`，服务端可独立复算）。口径不变：端点与 key 一律配置注入——窗口从环境变量 `SOAR_SUBTITLE_ENDPOINT` / `SOAR_SUBTITLE_API_KEY` / `SOAR_SUBTITLE_TIMEOUT_MS` 读入，未配置即恒空候选、零网络请求，仓库与日志永不含 key。UI 字幕菜单在 sidecar 候选之后追加 `[download]` 段（按 uri 只检索一次并缓存，本会话已下载的不再列出），选中即下载 → 落盘系统临时目录 `subtitles/`（`storeExternalSubtitle`，标题净化防路径穿越）→ 走 sidecar 同一条加载管线，失败只落 "Download failed" toast。所有失败分支（未配置、不可达、超时、HTTP 非 2xx、应答不可解析、媒体不可读）按接口契约静默降级为无候选/false，媒体照播。测试全部打本地 fixture HTTP 服务器（`test_http_servers.h` 的 catalog/empty/500/404/滞答/坏 key 模式 + 原始字节垃圾 socket），不打真实外网。
- **字幕文本翻译**：已有字幕轨/字幕文件时，把文本批量送 LLM/翻译 API，生成翻译字幕轨（轻量路径）。**核心路径与窗口接线均已落地（`SubtitleTranslator` + 字幕菜单的 `[translate]` 行，测试全离线）**：输入一段 SubRip/WebVTT 文本，cue 文本折叠成单行、按 `batch_cues`（默认 16）分块，以编号行（`1. text`）批量 POST 到用户自配的 OpenAI 兼容端点 `{endpoint}/chat/completions`（`model`/`temperature:0`/`messages` 三字段的最小公共子集，key 走 `Authorization: Bearer`；只支持 `http://`，TLS 与 http_cache 一样明确范围外）；应答必须给出与批次**逐行编号、条数一致**的译文，缺行、掉编号、非 2xx、超时、不可达、未配置（endpoint 或 model 空）一律 false 并带原因——**全有或全无**，绝不产出半翻译字幕轨。输出镜像输入格式（srt→srt、vtt→vtt），时间轴与 cue 编号原样保留，经 `storeExternalSubtitle` → `loadExternalSubtitle` 即成为一条真实字幕轨。应答 `choices[0].message.content` 的 JSON 转义（含 `\uXXXX` 与代理对，孤代理落 U+FFFD）由最小手写解码器处理，不引 JSON 库。**窗口接线**：字幕菜单与 Subtitle Settings 浮层在 `[download]` 段之后追加 `[translate] <轨名> -> <语言>` 行——只列本窗口自己加载过的外部轨（窗口记有源路径；内嵌轨无文件来源、不提供，直译「内嵌轨先导出」是后续片），每个轨每会话只译一次；点选即读文件 → 批量翻译 → 存盘 → 走 sidecar 同一条加载管线，失败只落 "Translation failed" toast、原轨不受影响。口径同下载源：端点/key/model/目标语言全由环境变量注入（`SOAR_TRANSLATE_ENDPOINT` / `SOAR_TRANSLATE_API_KEY` / `SOAR_TRANSLATE_MODEL` / `SOAR_TRANSLATE_TIMEOUT_MS` / `SOAR_TRANSLATE_LANGUAGE`，目标语言默认 "English"，窗口自身无语言选择器——一种语言够入口切片用），endpoint 或 model 未配置即不渲染任何翻译行、零网络；key 不进仓库与日志。请求形状与端到端链路（菜单点选 → 下载轨 → 翻译轨 id 递增）由本地 fixture chat 服务器逐字段断言（`test_http_servers.h` 的 chat 模式 + `requests.log` 请求录制 + Xvfb 窗口化注入用例），不打真实外网。
- **语音实时翻译**：无字幕轨媒体走 ASR（本地 whisper 类模型或云端）+ 翻译 + 字幕渲染（重量级路径，放最后）。
- **外挂 SRT/WebVTT 经 libass 默认样式 ✅（批 1c）**：有 libass 的构建里，选中外挂 SRT/WebVTT 轨不再走 UI 文本绘制——`loadExternalSubtitle` 在加载期就把该轨的 cues 合成为一篇默认样式的 ASS 文档（`synthesizeAssDocument` 纯函数：白字黑边、底缘居中、字号随视频高度缩放（288p 得经典 20px、1080p 得 77px）、PlayRes 跟视频、视频尺寸未知时回落 ASS 自带的 384×288；cue 文本除换行→`\N` 外逐字节保留），走批 1b 的同一条 `loadDocument` 画布路径。**依赖与自研边界**：字形渲染是 libass 的，默认样式与合成是本仓库的；无 libass 的构建零改动（纯文本泵照旧）。**随之收敛的三件事**：①画布的 `renderAt` 现在吃「播放头 + 字幕同步偏移」，且 V 显隐开关同时作用于画布与 UI 文本——批 1a/1b 的画布此前两者都不理（对搬上画布的文本轨是回归风险，本批一并补齐，内嵌 ASS 与 .ass 文档同样受益）；②外挂文本轨选中即文档模式：解码线程丢弃内嵌 ASS 事件，UI 的纯文本绘制在文档激活期整体跳过（后端新增 `subtitleDocumentActive()` 查询；防双绘的门放在 UI 而非泵——泵本身的语义两种构建通用：测试直接拉帧照常驱动泵，产线唯一的拉帧点就是这扇门，门关则泵随停，可观察行为与泵旁路无异，差别只在代码路径不被构建劈开）——**批 1b 遗留的「文本 sidecar × 内嵌 ASS 流叠加」在 libass 构建里就此关闭**，且文档激活期队列里的内嵌文本帧同样不上屏（无 libass 构建仍叠加，见下）；③`disableSubtitles` 与选回内嵌轨的释放语义复用批 1b。**边界（如实）**：内嵌文本轨（mov_text/subrip 流）的 cue 由解码线程流式产出而非加载期整批在手，故不走加载期合成——自内嵌轨选择语义批起它们在**被选中时**经同一合成默认样式头接画布（零 cue 文档做头 + 每帧重组 Dialogue 事件，见上文已知边界段）；UI 字号滑条不作用于画布渲染的轨（样式归脚本/默认样式，与 .ass 文档同口径），同步偏移作用在渲染时刻（`renderAt(pos+offset)`）故重合成无必要。测试：`synthesizeAssDocument` 单测（脚本形状、PlayRes 回落、字号带、倒挂 end 默认时长、经 `assDocumentCues` 的往返 oracle）；媒体套件——sidecar 播放用例两种构建同断言（泵照常吐帧、时序来自文件；libass 构建里这份队列输出与画布是同一行字——防双绘的门在 UI 不在泵）、新增「文本 sidecar 接管内嵌 ASS 画布」用例（红基线 → 选 SRT → 画布非红有字形）；assdrive X11 用例加 V 键往返（画布显隐门控两臂）。

已知边界（内嵌轨选择语义批收口后的现状）：内嵌轨的「选择」是**解码语义**，不再是元数据——解码线程只处理被选中的（或未被取消的 open 默认）字幕流。open 时默认轨照旧解出进纯文本队列（「打开即有字幕」的既有行为与测试契约不变）；显式选中一条内嵌轨时，若它不是 open 时建解码器的默认轨，先做运行时字幕解码器切换（与音频轨切换同一 pending 交接契约，失败则选择失败、原轨照播），且有 libass 的构建里该轨的 cue 喂上画布——ASS/SSA 轨用它自己的 CodecPrivate 重挂流式轨（不依赖 open 时的 stash），文本轨（mov_text/subrip 流）挂一个合成默认样式头（`synthesizeAssDocument` 零 cue 文档，批 1c 同款样式：白字黑边、底缘居中、字号随视频高度），解码器吐裸 `SUBTITLE_TEXT` rect 时每帧重组成一条 Dialogue 事件（`assDialogueLineFromText` 纯函数，换行→`\N`，其余字节保留；解码器给哪种 rect 形状随版本而异——实测 FFmpeg 8 的 mov_text 吐的是 ASS rect，走逐字 feed 臂，重组臂服务于吐裸文本的形状）；选中 sidecar 或 `disableSubtitles` 时解码门关闭、字幕队列清空，内嵌流从此不再产帧——**批 1b/1c 遗留的叠加面（含无 libass 构建里文本 sidecar 与内嵌字幕流同队混排）就此在两种构建里同时关闭**。仍如实记录的边界：选中/切换后事件从解码器读位置续流，选中点之前的 cue 要等一次倒退 seek 重扫才显示（批 1b 边界，未变）；UI 字号滑条不作用于画布渲染的轨——内嵌文本轨一旦被选中即画布轨，与外挂文档/文本轨同口径。

架构约定：翻译/ASR 服务走 **OpenAI 兼容端点由用户自配**（base URL + key），核心不内置任何云厂商凭据，不做强制联网；所有 AI 能力都是可选插件式服务，离线场景一切照旧。

