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

| 维度 | 实测（ec01888，run 36074545750） | 门禁（`--fail-under-*`） |
|---|---|---|
| 行 | 93.2%（1338/1435） | 93.0%（容 ~3 行抖动） |
| 分支 | 82.1%（1026/1249） | 81.2%（容 ~3 分支抖动） |

分文件行覆盖：`main.cpp` 95%（137/144）、`ffmpeg_backend.cpp` 91%（1022/1112）、`null_backend.cpp` 100%、`player.cpp` 100%。

分文件分支覆盖：`main.cpp` 85%、`player.cpp` 90%、`ffmpeg_backend.cpp` 81%、`null_backend.cpp` 79%。

P2 自适应协议批（77ae353/b6a2541）是纯测试批，零产品改动，分母不变；HLS/DASH/Range-seek 用例在 coverage job 的真实执行顺带把绝对命中数推高——行 +1（`ffmpeg_backend.cpp` 1827/1923：HLS TS 帧的 `best_effort_timestamp == AV_NOPTS_VALUE` fallback 与 seek 失败处理弧）、分支净 +3（新命中 77/276/1339/1826/1827/1922，翻转出 Missing 的 737/1201 见下）。同轮出现三处**时序弧翻转**（此前覆盖、本轮 missing）：`main.cpp` 219（窗口纹理尺寸变化时的旧纹理 destroy 弧，X11 用例时序）、`ffmpeg_backend.cpp` 737（`handled_here` 块内 PositionChanged+StateChanged 复合 emit 弧）、1201（切轨 `decode_cv_` 谓词的 `audioTrackPending()` 弧）。三者均属事件/竞争窄弧的逐轮摆动，与 §1 记录的抖动同性质，不是回归。

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

### 3.5 测试基建教训：媒体 fixture 必须逐字节确定性

多段 h264 TS 流（分辨率/像素格式变化测试）最初用 `cat` 裸拼接字节：每段的 TS 连续性计数器在接缝处重开，demuxer 间歇性报 `Packet corrupt` 丢包——**同样的字节在同一个 CI 的不同 job 一个过一个挂**（runs 36005020299：build/asan 过、coverage 挂）。改用 concat demuxer + `-c copy` 重新封装后时间戳与计数器连续，解码全程零警告。凡 fixture 生成，交付前用 `ffmpeg -v warning -i <file> -f null -` 验到零输出为止。

测试断言也不得依赖实时解码速度：sanitizer/coverage 插桩让解码慢数倍，轮询窗口要么配 `setRate` 解除墙钟节流，要么按最慢构建留足余量。对不能改的产品代码（如窗口循环里的播放没有 setRate），从进程外驱动时（XTEST 注入按键）按解码时间等待：等待窗口取"预期进度 ÷ 最低解码速度"，而不是赌正常速度。

## 4. 结论

行 93.2% 与分支 82.1% 是**当前代码库在“不删防御代码、不写假用例、不做进程污染”前提下的真实上限**（逐条复核结论：剩余缺口全部落入 §3.1-§3.4 之一定性，其中队列溢出与 drain 双队列比较两处由"产出-消费同线程串行"的结构论证支撑，见 §3.4；P1 看门狗新增 Missing 已随 7560c8f 归类闭环）。凑到字面 100% 只能：删掉防御分支、mock 掉被测库、或写不断言的假用例——三者都违背项目铁律。新增可测路径时按既有模式补测（真实文件、真实失败契约），并把门禁阈值随实测水位上抬。

可测路径的三个实用模式：

- **open 只为选中的流建 decoder**：因此“默认轨健康、非默认轨损坏”的容器能正常打开，把损坏的影响推到 selectTrack 时刻——双 AAC 轨只 patch 第二轨 CodecID（A_AAC→A_XXX）即可分别覆盖 open 失败与 selectTrack 失败两种契约（Error 态+事件 vs fail+旧 decoder 保留）。
- **seek 有两条路径，状态翻转只在同步路径**：Playing/Paused 态的 seek 委托给解码线程、由它播报状态；只有 Stopped 态（无解码线程）走调用线程上的同步 seekToTimestamp，才有“seek 精确 duration 翻转 Ended、从 Ended seek 回翻转 Paused”的重播报。测 seek 的状态语义必须选对路径。
- **SDL 窗口块的三条无头路径**：`SDL_VIDEODRIVER=dummy` 永远不提供加速渲染器，而产品代码显式请求 `SDL_RENDERER_ACCELERATED`——渲染器失败分支因此全平台确定性可达（干净退出，gcov 落盘）；真正的渲染循环用 Xvfb + python-xlib 驱动到干净退出，出口有两种，都不需要窗口管理器：XTEST 注入 Escape（需显式 `XSetInputFocus` 补上缺失的输入焦点），或直接向窗口发 `WM_DELETE_WINDOW` ClientMessage——SDL 自己监听 WM_PROTOCOLS 并把它转成 `SDL_QUIT`，这曾是文档里"QUIT 无法从进程外注入"的错误结论，后被本地 Xvfb 实证推翻并用例覆盖。“无头环境覆盖不了 GUI 代码”不成立，成立的只是“覆盖不了需要真实交互语义的分支”。
