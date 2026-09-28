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
- 加载外部字幕（sidecar）：`loadExternalSubtitle` 把媒体同目录的 `.srt`/`.vtt` 挂成一条字幕轨并可选中，字幕菜单与 Subtitle Settings 浮层列出候选（见 §6）

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
| **P4 P2P** | libtorrent（BSD，合规无冲突）做 piece 顺序优先下载；或本地 HTTP 代理桥接（BT 流伪装成 `http://localhost:port/`，FFmpeg 后端零改动） | 高 |

架构约定：**核心抽象不动**——网络能力是后端实现细节，唯一的核心层变更是 P1 的事件模型扩展；P3 起的缓存/下载层做成独立组件，`IBackend` 之上可组合，不绑死 FFmpeg。

## 6. 字幕生态（下载与 AI 翻译）

依赖顺序：先落地 v0.2 的字幕渲染基础（字体/大小/偏移），再接外部字幕源。

- **内嵌 ASS/SSA 样式渲染**：内嵌 ASS/SSA 轨不再降采样成纯文本——样式/字体/颜色/定位经 **libass**（FFmpeg 生态成熟的开源 ASS 渲染库，Linux 走 apt `libass-dev`、mac/win 走 vcpkg `libass` port）渲染。**依赖与自研的边界**：渲染本体全部是 libass 的能力；本仓库自研的是接线与合成——`AssRenderer` 薄封装组件（libass 头不进公共头；把 libass 的字形位图按 straight-alpha src-over 合成为一整张 RGBA 画布，`changed` 语义让纹理只在画面变化时重新上传；内部互斥让解码线程喂事件与 UI 线程渲染共享一个实例，`available()=false` 的无 libass 构建自动退回既有纯文本路径，与 SDL2/FFmpeg/ImGui 同一套可选能力降级口径）；FFmpeg 后端打开时注册 Matroska 附件字体（`ass_add_font`）、把 CodecPrivate（脚本头+样式）喂给流式轨，并把 libavcodec 的 ASS 载荷重组回 libass 要的完整 `Dialogue:` 行——libavcodec 的 ass 解码器给的是 8 逗号 `ff_ass_get_dialog` 形式（无 `Dialogue:` 前缀、无起止时间，时长在 Matroska BlockDuration / 包 duration 上），按「rect 字段 + 包 pts/duration」拼回 10 字段完整行是接线的关键一步；UI 在视频 letterbox 之后直接 blend 画布纹理（无 ImGui 的裸窗口路径同样生效）。字体解析固定 `ASS_FONTPROVIDER_NONE` + 容器附件字体：无 fontconfig、无系统字体枚举，渲染确定性可测——测试夹具用仓库内附的 Noto Mono（SIL OFL 1.1，`tests/fixtures/FONT-LICENSE.txt`）并逐像素断言「每个可见像素恰为脚本声明的纯红色」。seek 倒退时 `ass_flush_events` 清事件，防止解码器重扫造成的重复叠加。已知边界（同下文内嵌轨口径）：样式渲染作用于打开时选中的那条内嵌 ASS 轨；SRT/WebVTT 外挂与文本提取路径不受影响（非 ASS 流仍走原队列）。
- **字幕下载**：按媒体哈希/文件名从 OpenSubtitles 等公开源检索、下载、与轨道对齐；做成可插拔的 `SubtitleProvider` 接口（核心定义接口，实现可换源），失败静默降级为无字幕。**接口、本地 sidecar 实现与播放链路已落地**：`SubtitleProvider` 只有 `findCandidates`（列候选）与 `fetch`（取文本）两个方法，`SidecarSubtitleProvider` 在媒体同目录找同名 `.srt`/`.vtt`（支持 `movie.en.srt`、`movie.en.forced.srt` 这类语言/forced 标签，大小写不敏感），SRT 与 WebVTT 的文本解析是无依赖纯函数、容错口径写在头注释里（BOM、CRLF/裸 CR、缺小时字段、1-6 位小数、WebVTT 的 NOTE/STYLE/头部块、坏块跳过继续等）。`IBackend::loadExternalSubtitle(path, out_id)` 把一个 sidecar 挂成一条真正的字幕轨：外部轨 id 从容器流数起算（`nb_streams + slot`），永不与内嵌轨冲突，轨名是文件名、codec 按内容判为 `subrip`/`webvtt`；`selectTrack` 相应放宽到接受这个 id。sidecar 没有解码线程，所以帧由播放头在 `tryGetSubtitleFrame` 里泵进**内嵌字幕同一个队列**（UI 渲染零改动、也分不清两者来源）：播放头倒退（seek / A-B 回绕 / 重播）时游标从头重扫，前进跳过大段时只入队最后 4 条。加载失败（无媒体 / 读不了 / 无 cue）只落 `lastError`、不发 Error 事件——sidecar 缺失是常态，媒体照播。UI 侧在字幕菜单与 Subtitle Settings 浮层里列出候选（只在菜单打开时枚举目录，无文件对话框），选中即加载并选中。**检索/下载源已落地（可插拔 HTTP 客户端，自定义行协议，不宣称兼容 OpenSubtitles 等现有服务 API）**：`ExternalSubtitleProvider` 用一个自含的小型阻塞 HTTP 客户端对话一个**文档化的极简行协议**——检索 `GET {endpoint}?size={字节}&hash={16 位 hex}&name={主文件名}`（key 走 `X-API-Key` 头），200 应答每行一个候选、TAB 分隔 url/语言/标题/扩展名、`#` 为注释行，只收 `http://` 候选（TLS 与 http_cache 一样明确不在范围内）；下载 `GET {候选 url}`，应答体须能被核心解析器识别为 SubRip/WebVTT 才算成功（200 的 HTML 错误页不会变成字幕轨）。哈希取「头尾各 64 KiB 的 u64 小端词求和 + 文件大小」（`mediaHashHex`，服务端可独立复算）。口径不变：端点与 key 一律配置注入——窗口从环境变量 `SOAR_SUBTITLE_ENDPOINT` / `SOAR_SUBTITLE_API_KEY` / `SOAR_SUBTITLE_TIMEOUT_MS` 读入，未配置即恒空候选、零网络请求，仓库与日志永不含 key。UI 字幕菜单在 sidecar 候选之后追加 `[download]` 段（按 uri 只检索一次并缓存，本会话已下载的不再列出），选中即下载 → 落盘系统临时目录 `subtitles/`（`storeExternalSubtitle`，标题净化防路径穿越）→ 走 sidecar 同一条加载管线，失败只落 "Download failed" toast。所有失败分支（未配置、不可达、超时、HTTP 非 2xx、应答不可解析、媒体不可读）按接口契约静默降级为无候选/false，媒体照播。测试全部打本地 fixture HTTP 服务器（`test_http_servers.h` 的 catalog/empty/500/404/滞答/坏 key 模式 + 原始字节垃圾 socket），不打真实外网。
- **字幕文本翻译**：已有字幕轨/字幕文件时，把文本批量送 LLM/翻译 API，生成翻译字幕轨（轻量路径）。**核心路径与窗口接线均已落地（`SubtitleTranslator` + 字幕菜单的 `[translate]` 行，测试全离线）**：输入一段 SubRip/WebVTT 文本，cue 文本折叠成单行、按 `batch_cues`（默认 16）分块，以编号行（`1. text`）批量 POST 到用户自配的 OpenAI 兼容端点 `{endpoint}/chat/completions`（`model`/`temperature:0`/`messages` 三字段的最小公共子集，key 走 `Authorization: Bearer`；只支持 `http://`，TLS 与 http_cache 一样明确范围外）；应答必须给出与批次**逐行编号、条数一致**的译文，缺行、掉编号、非 2xx、超时、不可达、未配置（endpoint 或 model 空）一律 false 并带原因——**全有或全无**，绝不产出半翻译字幕轨。输出镜像输入格式（srt→srt、vtt→vtt），时间轴与 cue 编号原样保留，经 `storeExternalSubtitle` → `loadExternalSubtitle` 即成为一条真实字幕轨。应答 `choices[0].message.content` 的 JSON 转义（含 `\uXXXX` 与代理对，孤代理落 U+FFFD）由最小手写解码器处理，不引 JSON 库。**窗口接线**：字幕菜单与 Subtitle Settings 浮层在 `[download]` 段之后追加 `[translate] <轨名> -> <语言>` 行——只列本窗口自己加载过的外部轨（窗口记有源路径；内嵌轨无文件来源、不提供，直译「内嵌轨先导出」是后续片），每个轨每会话只译一次；点选即读文件 → 批量翻译 → 存盘 → 走 sidecar 同一条加载管线，失败只落 "Translation failed" toast、原轨不受影响。口径同下载源：端点/key/model/目标语言全由环境变量注入（`SOAR_TRANSLATE_ENDPOINT` / `SOAR_TRANSLATE_API_KEY` / `SOAR_TRANSLATE_MODEL` / `SOAR_TRANSLATE_TIMEOUT_MS` / `SOAR_TRANSLATE_LANGUAGE`，目标语言默认 "English"，窗口自身无语言选择器——一种语言够入口切片用），endpoint 或 model 未配置即不渲染任何翻译行、零网络；key 不进仓库与日志。请求形状与端到端链路（菜单点选 → 下载轨 → 翻译轨 id 递增）由本地 fixture chat 服务器逐字段断言（`test_http_servers.h` 的 chat 模式 + `requests.log` 请求录制 + Xvfb 窗口化注入用例），不打真实外网。
- **语音实时翻译**：无字幕轨媒体走 ASR（本地 whisper 类模型或云端）+ 翻译 + 字幕渲染（重量级路径，放最后）。

已知边界：sidecar 与内嵌字幕共用一条队列，而解码线程始终解内嵌字幕流（内嵌轨的"选择"至今只是元数据，不控制解码）。所以给一个**同时带内嵌字幕轨**的媒体选 sidecar 时，两边字幕可能先后都出现在屏幕上。真正的修法是让解码线程按 `selected_subtitle` 决定是否处理字幕包，属于内嵌轨选择语义的一并改造，不在本批范围。

架构约定：翻译/ASR 服务走 **OpenAI 兼容端点由用户自配**（base URL + key），核心不内置任何云厂商凭据，不做强制联网；所有 AI 能力都是可选插件式服务，离线场景一切照旧。

