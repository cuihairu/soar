# 覆盖率口径与未覆盖项清单

> 约定：本文记录 CI coverage job 的统计口径、当前水位、门禁阈值，以及**如实定性为不可达或版本依赖**的未覆盖项。目标是让“覆盖率不回退”有据可查，而不是追求字面 100%。

## 1. 统计口径

CI 的 coverage job（Ubuntu，GCC `--coverage` + gcovr）统计 `src/` + `include/`：

- gcov 原始分支记录约 30% 是 libstdc++ 内联代码的异常处理（EH）边，数字无意义，因此分支统计开 `--exclude-throw-branches` 过滤；
- 解码线程会让 gcov 的分支计数变负（GCC bug #68080），用 `--gcov-ignore-parse-errors negative_hits.warn_once_per_file` 忽略；行数据不受影响；
- 多线程计数噪声：忽略负值后个别行计数偶发归零，覆盖率对比时 ±1-2 行抖动**不是回归**；
- **计数损坏会伪装成"行缺失"**：窗口 CLI（解码线程 + 主循环 + SDL 线程）里，纹理重建的 212/215 两行曾**连续两轮**报 missing，但控制流证明它们必然执行过（213/214 覆盖而 212 缺失在 -O0 精确计数下不可能成立；CLI 事件流的 position 推进到 4333ms 证明第二/三分辨率帧确实发布过）。第三轮计数正常即显形覆盖。定性行缺失时先做控制流一致性检查，必要时用被测进程的事件流交叉验证，不要把假缺失定性成不可达。

FFmpeg 后端只在装了 libav* dev 头的环境编译，所以这个 Linux job 是 `ffmpeg_backend.cpp` 覆盖数字的唯一来源。

## 2. 当前水位与门禁

| 维度 | 实测（P3a 补测批，见下） | 门禁（`--fail-under-*`） |
|---|---|---|
| 行 | 94.6%（2484/2625） | 93.9%（容 ~18 行抖动） |
| 分支 | 83.2%（2228/2677） | 82.1%（容 ~30 分支抖动） |

分文件行覆盖：`main.cpp` 98%（112/114）、`ui_state.cpp` 100%、`ui_state.h` 100%、`player_window.cpp` 96%（615/640）、`ffmpeg_backend.cpp` 91%（1081/1189）、`http_cache.cpp` 99%（407/410）、`null_backend.cpp` 99%、`player.cpp` 100%。

分文件分支覆盖：`main.cpp` 94%、`ui_state.cpp` 96%、`ui_state.h` 100%、`player_window.cpp` 85%、`player.cpp` 89%、`ffmpeg_backend.cpp` 79%、`null_backend.cpp` 75%、`http_cache.cpp` 85%（436/511）。

### 2.1 门禁重置的说明（必读，别当成"新代码拉低了覆盖率"）

本批（桌面 UI，见 [ui-design.md](ui-design.md)）**没有**拉低覆盖率，反而把两个总数都抬高了；真正把水位压下来的是上一批 P3a 磁盘缓存，而它**没有**同步门禁：

| 口径 | 剔除本批新文件（`src/app/ui/*`）后 | 含本批 |
|---|---|---|
| 行 | 89.3%（1693/1895） | 91.5%（2402/2625） |
| 分支 | 72.9%（1354/1857） | 77.1%（2064/2677） |

也就是说 128499e（P3a，`http_cache.cpp` 410 行 / 511 分支）落地时门禁还停在 93.0/81.2，`coverage` job 从那一刻起就已经是红的——门禁不再拦截任何东西，只是恒红。P3a 的缺口定性（大量 HTTP/磁盘失败分支只可由"坏响应的本地 server"或注入构造）与 §3.1-§3.4 同族，单独补测是另一个批次的活，不在本批范围内。

因此本批按 §2 末尾"门禁随实测水位同步"的惯例**重置**门禁到实测水位下方（行 90.8 / 分支 76.0，余量取 §1 记录的抖动量级：多线程计数损坏 ±1-2 行、窗口/解码并发下时序弧逐轮翻转 ~1 个百分点），并把新代码的缺口逐条写进 §3.6。后续 P3a 补测批把 `http_cache.cpp` 拉起来之后，门禁按惯例再上抬。

P3a 补测批（本批）兑现了上面这句话：`http_cache.cpp` 行 80%→99%（407/410）、分支 55%→85%（436/511），总体水位 行 91.5%→94.6%、分支 77.1%→83.2%，门禁随之抬到 93.9/82.1（余量维持原绝对抖动量）。驱动方式是新增一台"行为不端的原始 socket server"脚本（`tests/test_http_servers.h` 的 `kMisbehavingServerScript`，12+ 个模式：早断连、垃圾状态行、64 KiB 超长头、重定向、chunked、1xx、截断 body、好探针坏拉取、畸形 Content-Range 五形态循环），加上 meta 文件逐字段篡改（11 个变体各对应一个 loadMeta 拒绝检查点）、RLIMIT_FSIZE 注入 `fdTruncate`/`fdWriteAllAt` 失败、数据文件从位图背后消失、meta 路径被目录占用等磁盘侧破坏。批内还修了一个测试揭出来的产品 bug：`parseHttpUrl` 曾把 IPv6 字面量的方括号留进 host 喂给 `getaddrinfo`（它不认带括号写法），导致所有 `http://[::1]:port/...` URL 恒报 "cannot resolve"；修复后 host 去括号、Host 头保留括号形式（RFC 3986 §3.2.2），IPv6 回环 Range 服务器上有字节级回归用例。残余缺口定性见 §3.7。

UI 自身的两个文件是全仓库最高的一档（`ui_state.*` 行 100%、`player_window.cpp` 行 96% / 分支 84%，均高于 `ffmpeg_backend.cpp` 与 `http_cache.cpp`），驱动方式是 §4 末尾那条"无头环境驱动 GUI"的模式：Xvfb + XTEST 脚本化会话（键鼠全家族、拖放以外的指针手势、弹层与 MRU 重开、窗口压到 1x1 的退化几何、慢速 server 下的缓冲指示）。

RTSP 冒烟批（5695bc7/c899a89）含一个小的产品修复：直播流负 duration 归 0 + seek 对 `duration <= 0` 拒绝（防 clamp UB）——`ffmpeg_backend.cpp` 行分母 +4（1027/1116，新行全被 RTSP 用例命中）、分支分母 +7（1029/1256）。同轮 `main.cpp` 216 行翻转出 Missing（X11 窗口纹理区，与 219 同族的时序弧逐轮摆动）、main 分支 85%→83%（同族纹理弧摆动 -2）；`test_backend_media` 的 lifecycle 用例把 300ms 裸睡改为轮询（§3.5 禁赌墙钟的又一实例，断言不变）。门禁维持不动：行余量 ~4.5、分支余量 ~9。

P2 自适应协议批（77ae353/b6a2541）是纯测试批，零产品改动，分母不变；HLS/DASH/Range-seek 用例在 coverage job 的真实执行顺带把绝对命中数推高——行 +1（`ffmpeg_backend.cpp`：HLS TS 帧的 `best_effort_timestamp == AV_NOPTS_VALUE` fallback 与 seek 失败处理弧）、分支净 +3（新命中 77/276/1339/1826/1827/1922，翻转出 Missing 的 737/1201 见下）。同轮出现三处**时序弧翻转**（此前覆盖、本轮 missing）：`main.cpp` 219（窗口纹理尺寸变化时的旧纹理 destroy 弧，X11 用例时序）、`ffmpeg_backend.cpp` 737（`handled_here` 块内 PositionChanged+StateChanged 复合 emit 弧）、1201（切轨 `decode_cv_` 谓词的 `audioTrackPending()` 弧）。三者均属事件/竞争窄弧的逐轮摆动，与 §1 记录的抖动同性质，不是回归。

7560c8f 的 P1 看门狗给 `ffmpeg_backend.cpp` 增加了 28 行可计行数，其中 20 行被慢速 server 用例命中（Started/Ended/EXIT 分辨/时间戳路径），新增 Missing 仅 2 行（1267-1268，定性见 §3.4），分母扩张导致百分比微降——绝对命中数与定性结论未回退，门禁维持不动（行余量 ~45、分支余量 ~9，按最差轮仍稳）。

余量按观测到的最差轮取：窗口测试的并发解码负载会让 backend 时序弧逐轮翻转（实测分支低点 988、行低点 1312），而 gcov 多线程计数损坏（见 §1）曾把已执行的行连续两轮报成 missing。门禁卡在低点之下、正常轮之上。

门禁随实测水位同步上抬：测试合入、数字上涨后，把阈值抬到“实测值下方留出抖动余量”的位置，让回退被 CI 自动拦截。

## 3. 未覆盖项定性（如实记录，不写假覆盖）

### 3.1 结构性不可达（由语言/库实现决定）

- **libstdc++ SSO（短字符串优化）内联分支**：字面量短串使长串路径结构性不可达（`null_backend.cpp` 29/47、`player.cpp` 66）。
- **内联/行号归属噪声（约 50 行）**：gcov 把内联库代码的执行记到邻近源码行（函数出口 `}`、调用密集行），两侧语义已覆盖但仍报 missing。

### 3.2 防御性代码（注入即污染）

- **OOM 分支（约 41 行）**：`av_mallocz` / `av_frame_alloc` / `SDL_AllocAudioStream` 等失败路径，只能用 interpose/mock 注入失败，会污染真实库行为，与“测真实链路”矛盾。
- **SDL 音频设备打开失败（8 行）**：`ensureOpen` 把声道数钳制到 1-8（`ffmpeg_backend.cpp:147`）、频率有 `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`，合法参数下 `SDL_OpenAudioDevice` 只有在无音频设备时才失败；dummy 驱动永不失败。构造真实拒绝（如 16 声道文件）会被 clamp 化解。
- **不变量短路弧（分支残余）**：`streams[id]` 存在则 `codecpar` 必存在（849 的复合条件中段）、选中的流必有已建 decoder（380-382 的 `&& decoder_` 侧）、解码帧的 pts 恒有效（1308/1339/1625 的 `AV_NOPTS_VALUE` 三元）——都是容器/解码器契约保证下不可达的防御弧。

### 3.3 版本依赖行为（随 FFmpeg 版本翻转，断言不钉阶段）

经验来源：runs 35941543676、35942391836 两次翻车。**凡“损坏文件在 demux/decoder 内部走哪条失败路径”的断言都是版本依赖的**，只有“open 失败 + Error 态 + 可恢复”的契约跨版本稳定。本地（FFmpeg 8）与 CI（FFmpeg 6.1）行为差异实例：

- **解码器拒收阶段**：容器声称 mpeg4、payload 是 h264 时，8 在 `receive_frame` 报错（触发 1302 行 fatal），6.1 在 `send_packet` 就拒收（走 1291-1292 / 1322-1323 行 continue，自然播到 EOF）。测试断言只要求“终结性”（Ended 或 Error，不挂死、可恢复）。
- **截断容错阶段**：512 字节截断的 mkv，8 接受 demuxer open、失败于 find_stream_info（1065 行），6.1 在 demuxer open 就拒绝。断言只钉“open 路径失败”。

### 3.4 场景不可构造（成本/稳定性不成比例）

- **`av_read_frame` 失败（1282 行）**：本地文件的 demuxer 把一切结构损坏宽容化为 EOF；网络流断开原记录为"需要起本地服务并中途 kill，成本不成比例"。随网络播放路线（docs/mvp.md §5 P0）启动，本地 HTTP 服务 fixture 已进入测试基建（P0 冒烟用例覆盖 http URL happy path），断流中途注入在后续网络功能深入时补测。
- **`AVERROR_EXIT` 出口（现 1270 行，竞争窄弧——2026-09-24 修正定性）**：此前记录为"无触发源"是**错误证伪**：产品代码在 open 时设置了 `interrupt_callback`（`ffmpeg_backend.cpp` openContext），stop() 置位后 FFmpeg 会中止阻塞中的 `av_read_frame` 并返回 `AVERROR_EXIT` → 1270 行静默 break（不报错，与 EOF/错误分支并列的第三种退出，stop 打断慢 IO 的正确语义）。之所以至今 missing：本地文件的 `av_read_frame` 微秒级返回，stop 恰落在阻塞中的窗口极窄，现有数百次 stop 序列零命中。P1 看门狗（7560c8f）后 EXIT 分支还承载超限中止（1267-1268，用 `network_stall_exceeded_` 标志与 stop 区分），慢速 server 用例 stop 时数据早已恢复、read 不阻塞，EXIT 弧仍零命中；网络/慢介质流下窗口变宽，随网络路线深入自然覆盖。
- **网络卡顿超限放弃出口（1267-1268 行，7560c8f 新增）**：看门狗 60s 容忍窗（`kNetworkStallLimitMs`）的放弃出口——stall 超过容忍窗才中止读并 fatal 进 Error。慢速 server fixture 的停顿（40s）**故意低于**容忍窗以钉"缓冲上报且自愈"契约；触发 1267-1268 需要 stall > 60s 的用例（时间成本 +60s 起），而"终结于 Error 态、可恢复"的契约已有独立用例钉死，再加长停顿用例属于重复覆盖，成本不成比例。
- **seek 内部失败的 Error 事件（722/732-733/1187/1771/1830-1837 行）**：需要 `avformat_seek_file` 在可 seek 的本地文件上失败——它对合法位置总是成功；不可 seek 的流在 `seek()` 更早的分支就被直接处理，走不到这里。`applyPendingAudioTrack` 的 seek 失败回滚（1830-1837）与 `seekToTimestamp` 自身的失败出口（1771）同源。注入自定义 AVIO 才能命中，超出测试基建范围。
- **stop 中断 read 的时序分支（39/42 行）与 decodeLoop 的 stop break（1167-1168 行）**：都要求在解码线程恰好处于特定等待点时打断它。1167-1168 已做专门证伪：Paused 态线程恒驻 `waitForPresentationTime` 内部的 wait（1393/1409），所有 stop 路径经其返回 false → drain 循环条件 → decodeLoop 循环条件退出，1167-1168 被结构性跳过；唯一命中窗口是 pause/stop 恰落在"线程回 1159 且谓词已为 false"的微秒级竞争里，现有数百次 stop 序列（headless CLI、pause 用例、open/close storm）零命中。时序敏感，flake 风险大于覆盖收益。
- **帧队列溢出丢帧（1319-1321/1337-1339 行，结构性）**：解码产出与 drain 消费在**同一个线程**串行（1296 每包调用 drain，drain 阻塞消费期间解码线程自己也被阻塞），而每个 packet 在 send/receive 循环里至多产出 1-2 帧（B 帧延迟跨包累计 ≤3）——队列深度物理上到不了 6/32 的上限。只有把产出挪到独立线程才会可达。
- **drain 的双队列比较块（1353-1356 行，结构性）**：同一串行模型的推论——drain 退出即双空，每次进入 drain 前只入队一个 packet 的帧，因此进入时**至多单侧非空**，"双队列都非空才走"的比较块不可达。
- **`main.cpp` 的 SDL 窗口块残余（96% 行 / 85% 分支）**：窗口块主体已被三条确定性路径覆盖——dummy 视频驱动必然无加速渲染器（renderer 失败分支）；Xvfb + XTEST Escape 驱动渲染循环到干净退出；WM_DELETE_WINDOW ClientMessage 驱动 `SDL_QUIT` 分支（SDL 自己监听 WM_PROTOCOLS，无需窗口管理器）与 null 后端下的 `ffmpeg_backend` 短路弧、空纹理清理弧。行残余缺口：`SDL_CreateWindow` 失败（175-177，dummy/Xvfb 下建窗必成功）、`SDL_CreateTexture` 失败打印（225，合法尺寸不失败）、212/215（已证伪的 gcov 假缺失，见 §1）。分支残余：纹理重建链的短路弧与 `SDL_QueryTexture` 失败侧（重建时旧纹理已损坏，不构造）、窗口/纹理创建失败的防御弧、事件回调链的 114 弧（open 失败的 Error 事件本体已被 asset:// 用例覆盖，残余弧属多线程回调的计数损坏/归属噪声家族，见 §1）。
- **`main.cpp:130` 的 seekable "no" 弧（已覆盖，fdd797e）**：CLI 的常规媒体来源——本地文件（FFmpeg 后端）与 null 后端的模拟媒体——`mediaInfo().seekable` 都为 true。此前定性为"可构造但成本不成比例"（当时只想到 /dev/stdin 喂送方案）。网络冒烟用例（fdd797e）用更轻的构造命中了它：python `http.server` 不应答 Range，FFmpeg 对该源报 seekable=false，headless 流程的 seek 走 130 的 "no" 分支。
- **状态快照的时序弧（522/528/958/963 行）**：play() 的 Ended/Error 重播报组合与 selectTrack 收尾的 state!=Stopped 判断，都是线程 wind-down 与状态快照竞争的窄弧，逐轮翻转（门禁余量按此取值），不构造。

### 3.6 桌面 UI（`src/app/ui/*`）的未覆盖项

`ui_state.*` 行覆盖 100%，缺口只在 `player_window.cpp`（行 96% / 分支 84%），全部落在下面四类。X11 驱动会话（`soar_cli_tests` 的 drive/stall 两例）已覆盖：全部快捷键族、滚轮两向、单击/双击、seek 条悬停/拖动/提交、OSC 组合菜单的选中与取消选中、音量条拖动、三个浮层的开关、MRU 的重开成功/重开失败、缓冲指示、1x1 退化几何、bare 窗口降级路径（CI `no-imgui` job）。

- **SDL 资源创建失败的防御弧（119-120/127/954-956 行）**：`SDL_CreateTexture` / `SDL_UpdateYUVTexture` / `SDL_CreateWindow` 的失败分支。Xvfb 与 dummy 驱动下建窗建纹理必成功，合法尺寸下纹理更新不失败——与 §3.4 里 `main.cpp` 旧窗口块同族（那批代码搬到了本文件），注入 SDL 失败等于 mock 被测库，不做。
- **`SDL_DROPFILE` 整块（359/361-365 行）**：拖放打开需要真实的 XDND 拖拽源（拖拽发起方的 selection 协商），XTEST 只能合成键鼠指针事件，构造不出跨进程 DnD 会话。同一 `openSource` 目标函数已由 MRU 重开路径覆盖（成功与失败两条），缺的只是"事件从哪来"。
- **枚举 switch 的兜底与 Error 臂（59/61/70 行）**：`stateName`/`trackTypeName` 的 `return "?"` 是穷尽 switch 的编译器要求，实参取不到枚举外的值；`case PlaybackState::Error` 需要后端把状态翻成 Error——`NullBackend::fail()` 只置错误串不动状态，FFmpeg 后端的 fatal 解码路径才翻（版本依赖，见 §3.3），窗口会话跑在 null 后端上。
- **切轨/Seek 失败的 toast（427/493/502/508-509/511/712 行）**：`Player::seek` / `selectTrack` 返回 false 的分支。null 后端对已打开的媒体恒返回 true（无轨可循环时走的是更早的"没有该类轨"分支，已由 stall 会话的纯音频 fixture 覆盖 Subtitle 侧），FFmpeg 后端要构造真实 IO 失败或坏轨 codec（§3.3/§3.4 的版本依赖与注入污染家族）。508-509 还多一层条件：需要**两条以上**字幕轨才会走"下一条"而不是"关掉"，null 后端只有一条。
- **seek 条的"取消拖动"臂（595 行）**：`IsItemDeactivated()` 且 `IsItemDeactivatedAfterEdit()` 为假。ImGui 的滑条在按下瞬间就会改值（点击即定位），所以"拖走再拖回原位"仍被记为 edited；唯一不改值的按法是**精确按在当前值的手柄上**（把手柄算到像素、且期间播放位置不动），属于坐标级脆弱构造，不做。已覆盖的是它的兄弟路径：悬停提示、拖动预览、松手提交。
- **字体候选全落空的内嵌回退（206 行）**：七个系统字体候选都不存在时才走到。X11 驱动用例用 `SOAR_UI_BITMAP_FONT=1` 钉住内嵌位图字体（这也是注入器坐标跨机器稳定的前提），因此该回退在测试里被短路；CI runner 实际带 DejaVu，走的是候选命中那条。
- **"No recent files yet."（850 行）**：窗口化流程里 MRU 永远非空——`PlayerHud` 构造即 `recordOpen(cfg.initial_uri)`，而 CLI 没有 URI 时根本进不到窗口。"空列表"的语义由 `soar_ui_tests` 的 `RecentStore` 单测覆盖。
- **窗口尺寸守卫的新增行**（`drawUi` 的 `DisplaySize < 1` 早退）由驱动会话的 `win.configure(1x1)` 命中：没有窗口管理器时 `XResizeWindow` 直接生效，SDL 拿到退化 drawable 后必须跳过整帧绘制（否则 OSC 宽度会算成负数）。

### 3.7 http 磁盘缓存（`src/core/http_cache.cpp`）的残余缺口

行 99%（407/410，唯一缺的是 saveMeta 短写弧，见下）、分支 85%（436/511）。缓存组件的"坏 HTTP 响应"与"坏磁盘状态"两大弧族已由原始 socket server 模式族与磁盘破坏用例确定性覆盖（见 §2.1 P3a 补测批段落），剩下的缺失臂全部落入以下四族，无一是"测一下就有"的：

- **saveMeta 短写（606-608 行，唯一缺失行）**：需要"fopen 成功但 fwrite 短写/失败"。注入手段只有文件系统故障或 RLIMIT_FSIZE，而后者被结构否定：fetchBlock 先写 256 KiB 数据块再写 ~40 字节的 meta，**允许数据块写入的配额必然允许更小的 meta 写入**，构造不出"meta 超限而数据块不超限"的窗口（ctor 的 fdTruncate 会先因配额失败）；`<meta>.tmp` 的路径形态也进不了 `/dev/full`。等价于需要真实 IO 故障注入层，超出"测真实链路"的边界。
- **结构死臂（分支，编译期/契约级不可达）**：`readWholeFile` 的 limit 判断先于 `ok` 求值（112）；`parseHttpUrl` 的 scheme 复查（130，ctor 已验证前缀）、空 authority 死臂（141，空值已在更早分支返回）、路径三元的一臂归属噪声（134，§1 家族）；`getaddrinfo` 返回 0 却无结果的契约不可能臂（205/207）；`socket()` 创建失败需 EMFILE 级资源耗尽（213）；`httpFetch` 的无 Range 请求臂（347，组件恒带 Range）；响应头扫描器的 npos 臂（370/371/373/382，`\r\n\r\n` 终止子由 recvHeaders 保证，文本里必然有行边界）；`read()` 的兜底空错误文案（685，ensureBlock 的每条 false 路径都设置了 last_error_）。
- **60 秒硬超时的文案臂（266/290-293 行分支）**：`kRecvTimeoutSec = 60` 是编译期常量且无注入口子（这是设计：看门狗进不了这个组件的 recv，超时是唯一的硬停）。命中超时文案需要真实等 60 秒，成本不成比例；超时**路径本身**（isTimeoutError 判定 + 断连文案）已由早断连/截断 body 模式覆盖。
- **平台臂**：`isTimeoutError` 的 `EWOULDBLOCK` 分量在 Linux 上与 `EAGAIN` 同值，第二比较结构性不可达（193）；Windows 专属行（winsock 初始化、`_lseeki64`/`_write`/`_chsize_s` 臂）只存在于 Windows 编译，不计入本 Linux 口径。

### 3.5 测试基建教训：媒体 fixture 必须逐字节确定性
多段 h264 TS 流（分辨率/像素格式变化测试）最初用 `cat` 裸拼接字节：每段的 TS 连续性计数器在接缝处重开，demuxer 间歇性报 `Packet corrupt` 丢包——**同样的字节在同一个 CI 的不同 job 一个过一个挂**（runs 36005020299：build/asan 过、coverage 挂）。改用 concat demuxer + `-c copy` 重新封装后时间戳与计数器连续，解码全程零警告。凡 fixture 生成，交付前用 `ffmpeg -v warning -i <file> -f null -` 验到零输出为止。

测试断言也不得依赖实时解码速度：sanitizer/coverage 插桩让解码慢数倍，轮询窗口要么配 `setRate` 解除墙钟节流，要么按最慢构建留足余量。对不能改的产品代码（如窗口循环里的播放没有 setRate），从进程外驱动时（XTEST 注入按键）按解码时间等待：等待窗口取"预期进度 ÷ 最低解码速度"，而不是赌正常速度。

## 4. 结论

行 94.6% 与分支 83.2% 是**当前代码库在“不删防御代码、不写假用例、不做进程污染”前提下的真实上限**（逐条复核结论：剩余缺口全部落入 §3.1-§3.4 之一定性——其中队列溢出与 drain 双队列比较两处由"产出-消费同线程串行"的结构论证支撑，UI 的缺口见 §3.6 的六类逐条定性，http 缓存的缺口见 §3.7 的四类逐条定性）。凑到字面 100% 只能：删掉防御分支、mock 掉被测库、或写不断言的假用例——三者都违背项目铁律。新增可测路径时按既有模式补测（真实文件、真实失败契约），并把门禁阈值随实测水位上抬。

可测路径的三个实用模式：

- **open 只为选中的流建 decoder**：因此“默认轨健康、非默认轨损坏”的容器能正常打开，把损坏的影响推到 selectTrack 时刻——双 AAC 轨只 patch 第二轨 CodecID（A_AAC→A_XXX）即可分别覆盖 open 失败与 selectTrack 失败两种契约（Error 态+事件 vs fail+旧 decoder 保留）。
- **seek 有两条路径，状态翻转只在同步路径**：Playing/Paused 态的 seek 委托给解码线程、由它播报状态；只有 Stopped 态（无解码线程）走调用线程上的同步 seekToTimestamp，才有“seek 精确 duration 翻转 Ended、从 Ended seek 回翻转 Paused”的重播报。测 seek 的状态语义必须选对路径。
- **SDL 窗口块的三条无头路径**：`SDL_VIDEODRIVER=dummy` 永远不提供加速渲染器，而产品代码显式请求 `SDL_RENDERER_ACCELERATED`——渲染器失败分支因此全平台确定性可达（干净退出，gcov 落盘）；真正的渲染循环用 Xvfb + python-xlib 驱动到干净退出，出口有两种，都不需要窗口管理器：XTEST 注入 Escape（需显式 `XSetInputFocus` 补上缺失的输入焦点），或直接向窗口发 `WM_DELETE_WINDOW` ClientMessage——SDL 自己监听 WM_PROTOCOLS 并把它转成 `SDL_QUIT`，这曾是文档里"QUIT 无法从进程外注入"的错误结论，后被本地 Xvfb 实证推翻并用例覆盖。“无头环境覆盖不了 GUI 代码”不成立，成立的只是“覆盖不了需要真实交互语义的分支”。
