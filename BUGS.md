# 已知缺陷台账（BUGS.md）

用户报障的登记与收口记录。每条记录现象、根因、排查过程、修复与验证；
如实写，不美化。新缺陷登记在末尾，收口后补验证证据。

---

## 1. Windows nightly 包解压后运行报「缺少 DLL」（已修复）

- **现象**：用户下载 nightly `soar-windows-x64.zip`，解压双击
  `soar.exe` 报缺少多个 DLL，无法启动。
- **根因**（三层，CI 复现坐实）：
  1. libtorrent 的 `find_package(OpenSSL)` 从未命中 vcpkg——
     `vcpkg.json` manifest 没声明 openssl → CMAKE_PREFIX_PATH 空 →
     模块落到打包机现成库（Windows 是 PATH 里的系统 OpenSSL），
     exe 导入的 `libssl-3-x64.dll` / `libcrypto-3-x64.dll` 根本不在包里；
  2. MSVC 运行库（`MSVCP140` / `VCRUNTIME140` / `VCRUNTIME140_1`）
     无人捆绑；
  3. vcpkg applocal 只搬它认得的 SDL2/fmt，其余全漏。
- **修复**：`vcpkg.json` 声明 openssl；`scripts/stage-windows-dlls.sh`
  按 dumpbin 依赖闭包递归捆 DLL；CI 打包与验证共用该脚本，闭包里
  resolve 不到的名字直接失败、不出不完整的包。
- **验证**：干净目录 + PATH 只留 System32 实跑 `--version` 退出码 0
  （runner 自带的运行库会把漏捆掩盖掉，收窄 PATH 后缺一个立刻现形）；
  nightly 37145221440 的包回传验证自包含。
- **提交**：dbd3db8 → ab67dd2 → 545bfe2 → 97f37ae（含 CI 退出码修复）。

## 2. Windows 编译产物双击运行「黑框一闪而过」（已修复）

- **现象**：用户双击 soar.exe，出现一个黑色控制台窗口一闪而过，
  看不到任何界面或报错。
- **排查**（逐项过常见根因，留痕）：
  - **缺 DLL**：与缺陷 1 并案，已由闭包捆绑修复——但修完仍闪退，
    说明另有主因；
  - **启动即 panic/异常**：代码库是 error_code 风格、无异常路径，
    启动早期失败全部走 `return` + stderr 打印，排除；
  - **工作目录/配置文件缺失**：状态文件（recent 列表等）读写全是
    best-effort（失败静默跳过），缺目录不崩，排除；
  - **控制台程序被当 GUI 双击**：**确认为主因**。`soar.exe` 是控制台
    子系统程序：资源管理器双击 → Windows 为它新开一个控制台 →
    `main` 打印 Usage/错误后 `return` → 控制台随进程退出瞬间关闭。
    「黑框」就是那个短命控制台；一切只写 stdout/stderr 的启动失败
    （含无参数的 Usage）在双击场景全部不可见。
- **修复**（2026-10，与包格式批并案）：
  1. **双前端**：`soar.exe`（控制台，CLI 行为不变）+ 新增
     `soarw.exe`（GUI 子系统，`WinMain` 入口，无控制台）——mpv 的
     mpv.exe/mpv.com 双二进制先例。安装包的快捷方式指向 soarw.exe；
  2. **启动错误可见化**（`src/app/startup_report.{h,cpp}`）：任何
     启动早期失败必须留痕，不许静默闪退——
     - 失败类（源打开失败、torrent 启动失败、SDL 初始化/渲染器失败）：
       追加一行带时间戳的记录到
       `%TEMP%\soar-startup-failures.log`（`SOAR_STARTUP_LOG` 可覆盖）；
     - 对话框（仅 Windows）：无可用交互 stdout 时弹出消息框——
       GUI 子系统双击（无控制台）必弹；控制台程序独占控制台
       （资源管理器双击）必弹；终端共享控制台、管道捕获（CI）、
       重定向到文件都不弹（打印即可见，弹框反而会挂起自动化）；
       `SOAR_NO_DIALOG=1` 一律豁免（CI 保险）；
     - usage 类（无参数/未知选项/未知后端/无源）：预期行为非失败，
       只弹对话框、不写日志。
  3. **CI 防回归**：ci.yml / daily-build 的 Windows 冒烟断言
     soarw.exe 的 PE 子系统 = 2（GUI）、`--version` 经句柄重定向可捕获、
     无参数（`SOAR_NO_DIALOG=1`）退出码 2，以及安装器实装后窗口站住
     （dummy 驱动 + 存活断言）。
- **验证**：本地 10 条退出路径回归 + 3 条新增日志/对话框测试绿；
  CI Windows 腿第一次整腿实跑（run 37169343362）在编译期抓出一个真错——
  `GetConsoleProcessList(2, attached)` 参数序写反（首参应为进程数组），
  MSVC error C2664 当场拦下（该行在 `#ifdef _WIN32` 块内，Linux 构建
  永远测不到；修复 7189e1b 经 mingw 交叉编译验证 API 签名层）；CI
  覆盖率读数（同 run）行 95.1%（5659/5952）、分支 85.18%（5797/6806）
  双过门禁，`startup_report.cpp` 行 27/27 = 100%；修复后 Windows 腿
  （PE 子系统校验/重定向捕获/窗口站住断言）与 nightly 安装器静默
  装→实跑→卸载走查由后续 CI run 终证。
- **残余（如实）**：`SDL_CreateWindow` 失败点（SDL_Init 成功后建窗
  失败，实践上极罕见）只打印不弹框不落日志——该路径无测试覆盖，
  为不突破覆盖率门禁未接线，留待后续测试收编批。
- **收口（2026-10-04）**：建窗失败分支已接线——`player_window.cpp`
  的 `if (!window)` 块与 SDL_Init/CreateRenderer 同款：print →
  `reportFatalStartupError`（Windows 下弹框 + 落
  `%TEMP%\soar-startup-failures.log`）→ `SDL_Quit` → exit 1，soarw.exe
  无控制台场景不再静默死。该分支的确定性端到端触发经实证不存在
  （SDL 2.32.10 探针走查）：建窗失败须 SDL_Init 成功而
  SDL_CreateWindow 返回 NULL——dummy / offscreen / x11（常规、1x1 屏、
  8bpp PseudoColor）下建窗恒成功；令 Init 失败的杠杆（bogus 驱动、
  断连 display、无效 `SDL_VIDEO_X11_VISUALID`/`SDL_HINT_VIDEO_X11_
  WINDOW_VISUALID`）全部落在 SDL_Init 点（已有测试覆盖）；
  RLIMIT_AS 扫描的失败断点在 SDL_CreateRenderer 阶段而非建窗；
  注入 SDL 失败等于 mock 被测库，按 coverage-notes §3.2 政策不做。
  故该分支维持"已接线、未覆盖"：接线在既已未覆盖的块内**只加
  分母 +2 行、零新增分支**，门禁数学成立。本地双树全量 8/8 绿 +
  gcovr 行 95.1%/分支 84.6% 过；CI 终证（run 37191653811，35ee683）
  行 5660/5954 = 95.1%、分支 5795/6811 = 85.09% 双过门禁，
  `player_window.cpp` 缺失行 2058-2062 与预测一致。

## 3. 安装版 soarw.exe 双击启动弹错「No media source given / Usage」（已修复，待真机复测）

- **现象**：安装版双击 soarw.exe（桌面/开始菜单快捷方式、资源管理器
  直接双击），弹出对话框「soar 0.1.0 / No media source given /
  Usage: ...」——用户双击打开一个播放器，得到的却是一段命令行用法。
- **排查**：
  - 与缺陷 2 同根、是它的续集：双前端修复让黑框与启动失败可见了，
    但**没区分「双击」与「终端调用」的语义**——`soarAppMain` 无参数
    一律走 usage 退出 2，`usage 类` 启动对话框（缺陷 2 修的「可见化」）
    把这段报错原样弹了出来。「双击是启动，不是命令行」这个边界从
    没有被编码；
  - 资源管理器「打开方式」链路同查：安装器（soar.iss）原本**没有任何**
    文件关联注册（连 OpenWithProgids 都没有），`.desktop` 是
    `Exec=soar` 无参（菜单点开 = usage 退出且 Terminal=false 什么都不
    显示）——「双击媒体文件打开播放」的链路两端都没接线，argv 传文件
    的那段行为是好的（所有 CLI 测试都在跑），缺的是入口注册与空启动。
- **修复**（2026-10-05，与 #2 并案收口）：
  1. **入口语义分叉**：`soarAppMain(argc, argv, gui_entry)`——GUI 前端
     （`winmain.cpp` 的 `WinMain`，命令行经 `CommandLineToArgvW` → UTF-8）
     与显式 `--gui` 旗标置 `gui_entry=true`；无源时只有
     `!gui_entry || headless`（终端调用/headless）才打 usage 退出 2。
     `--help/-h` 照打 usage 退出 0；未知选项照打 usage 退出 2——
     「参数非法才输出 usage」的口径不变。控制台 `soar.exe` 无参行为
     逐字不变（退出 2 + Usage）；
  2. **空主界面**（`player_window.cpp`）：GUI 无源启动直接进窗口——
     `firstRunEmpty()`（当前无源且非 torrent 等待态）显示「Open a file」
     海报 + 「Drop a file here - R recent - H shortcuts」提示，HUD 正常；
     打开入口三路：**O 键** / **点海报**（Windows 下
     `GetOpenFileNameW` 真文件对话框、comdlg32 链接；其他平台降级
     toast「No file dialog in this build - drop a file to open」）/
     **拖放**（原 SDL_DROPFILE 路径）。help overlay 增 `O` 行。
     有源启动（关联双击、命令行）不受影响，走原有播放流程；
  3. **默认后端改 ffmpeg**：无显式 `--backend=` 时 `SOAR_WITH_FFMPEG`
     → ffmpeg、未编入 → null。原默认 null 对任何 URI 都「成功」假播
     10 分钟黑屏——GUI 空窗后续打开的文件（对话框/拖放/最近）会静默
     黑屏假播，必须用能真解码的后端；`--backend=` 显式指定行为不变；
  4. **文件关联接线**（收口 ③）：
     - Windows：soar.iss 注册 `Soar.Media` ProgId + 18 个媒体扩展的
       `OpenWithProgids` 候选（mp4/mkv/…/m3u8/mpd/torrent）——只作
       「打开方式」候选**不抢默认**，打开命令 `soarw.exe "%1"`，
       卸载时 ProgId 整键删、候选值逐个删（无痕）；
     - Linux：`.desktop` 改 `Exec=soar --gui`（菜单点开不再 usage
       静默退出）+ `MimeType=video/*;audio/*;application/x-bittorrent;
       x-scheme-handler/magnet;`；
  5. **CI 同步改**：ci.yml / daily-build 的 Windows 冒烟「soarw 无参
     退出 2」断言改为「无参空启动 8 秒存活」（`SOAR_NO_DIALOG=1` 下
     真退了才判——exit 2 = usage 回归，唯一豁免仍与窗口站住断言同款：
     exit 1 + SDL_ 日志的渲染器缺失干净失败）；daily-build 安装器走查
     增断言：装后 `HKLM\Software\Classes\Soar.Media\shell\open\command`
     指向 soarw.exe、`.mp4\OpenWithProgids` 有 Soar.Media 候选；
     卸载后两者必须消失。
- **验证**：
  - 本地 CLI 冒烟 8 项：无参 exit 2 + Usage；`--help`/`-h` exit 0；
    `--gui --headless` 无源 exit 2（headless 是显式命令行语义）；
    默认后端打印 `Using FFmpeg backend`；`--version` 0；`--bogus` 2；
  - 本地 Xvfb 走查（SOAR_ENABLE_IMGUI=ON 树，display :103）：空窗海报
    「Open a file / Stopped / Drop a file here - R recent - H shortcuts」
    + 完整 HUD；按 O 出 toast「No file dialog in this build - drop a
    file to open」（Toast 800ms，首拍 0.8s 恰好过期，0.3s 重拍实锤）；
    H 帮助面板第 2 行 `O / Open a file (Windows dialog)`；Esc 退出；
  - 新增测试 4 条：`--help/-h` 退出 0、`--gui --headless` 无源保持
    usage 退出 2、默认后端解析为 FFmpeg（fixture 门控）、X11 空窗端到端
    （injector `empty` 模式：O 键 → 点击 → Escape，断言 exit 0 且无
    Usage/No media source 文本）；
  - 七腿 CI 终证：run 37231910426（提交 9b1e0d0）七腿全绿，行
    95.1%（5686/5980）、支 85.2%（5829/6845）——分母较上批
    （run 37191653811：5660/5954、5795/6811）+26 行/+34 支即本批
    新代码，门禁 95.0/84.6 双过；Windows 冒烟新断言（无参空启动
    存活）与 daily-build 安装器注册表走查随该跑生效。
- **残余（如实）**：Windows 真机复测待用户——安装版双击进空窗、
  对话框选文件、资源管理器「打开方式 → Soar」关联播放三条链路
  （CI runner 无交互桌面，文件对话框与关联双击无法自动化）；
  Linux 文件管理器的实际打开链路（desktop MimeType 声明面已接线）
  未在真桌面环境走查。

## 4. CLI 播放自然结束后进程偶发不退出（收尾段两处无界等待已修 c416163，根因待 CI 探针归因）

- **现象**：CI 集成测试里 `soar --headless` 子进程播完流媒体后不退出，
  被 `runCli` 的 `timeout` watchdog 杀成 exit 124，断言失败。四起：
  1. run 37575751946 tsan 腿（ab1cddf）：DASH case（test_cli.cpp:3998），
     44/45 过；
  2. run 37586409981 no-imgui fallback 腿（9d6ede5）：多变体 HLS case
     （test_cli.cpp:3937），有完整踪迹——事件流走到 `download 100%`、
     `event: state=3`（PlaybackState::Ended）之后进程未退出，挂到
     600s watchdog；
  3. run 37575751946 asan 腿同 run 的 `soar_p2p_tests` 600s 超时零踪迹
     （进程被杀时输出全在未 flush 的缓冲里）——是否同族（线程收尾挂死）
     无法归因，行缓冲 main 落地后下次复现有踪迹可判。
  4. run 38525153344 tsan 腿（083cebf）：RTSP live 用例（test_cli.cpp:6746）
     exit 124 挂满 600s——同族第四起、首例 RTSP；该 helper 未设
     SOAR_HANG_DUMP 故无栈。同 SHA rerun 七腿全绿=间歇性确认。已给
     runHeadlessOverRtsp 补 probe + 124 重试（镜像 HTTP 版三件套）。
- **已知事实**：headless 路径是 open→play→seek→pause→stop 直线序，
  没有自然结束等待循环（早版记档「主循环已按 main.cpp:322 break」
  有误——现行 main.cpp 322 行在 backend 构造块内，无此循环）。EOF
  处理必发 state=3（Ended），其后 stop() 收尾必发 Stopped+position：
  incident 2 的 trail 止于 state=3，坐实主线程停在 stop() 内（收窄
  论证见下 2026-10-14 晚间续审与 2026-10-15 段）。与 sanitizer 无关
  （第 2 起在无 sanitizer 的 fallback 腿）；与本批改动无关（两 commit
  分别只动 docs、测试日志缓冲、workflow）。间歇性：同 case 在多数
  run 秒级干净退出。
- **排查**：`--log-failed` 四起对比，第 2 起的完整事件流坐实「已 Ended
  未退出」；本地多次重放 HLS/DASH case 未复现（runner 特定时序）。
  2026-10-10 复查：headless 收尾链逐环审计（Player 薄封装、stop()/
  close() 先 join 后关 SDLAudio、SDLAudio 自带互斥、torrent_stream
  声明序先于 player 析构）未找到确定性缺陷；本地 70 次重放（hls_hi/
  master/dash，per-run server 复刻测试生命周期）零挂死。2026-10-14
  追加 36 次单核挤轧重放（taskset -c 0 覆盖构建跑 dash/master/media
  三 case 各 12 轮）依旧零挂死——runner 特定时序的假设再增强，
  仍等 SOAR_HANG_DUMP 在 CI 下一次挂死时交栈。
  2026-10-14 晚间续审（巡检）：RTSP 案补上此前缺位的本地重放——
  36 轮单核逐进程退出码复刻测试夹具（cov3 18 + tsan 12 + tsan-o0 6，
  全部 1-3s 干净退出），该 bug 本地累计 142 轮重放零复现。踪迹形态
  把挂点收窄进 stop() 本身：incident 2 的 trail 止于 state=3 且其后
  无 state=0——stop() 收尾必发 Stopped+position，故 return 0 之后的
  析构链（libtorrent 销毁、SDL 拆台）被排除，主线程停在 stop() 的
  join 或其后段。该段全等待面复核无确定性缺陷：读阻塞有中断回调
  （should_stop 即弃读，rtsp/udp 轮询环会再查）、循环顶与 pacing
  （waitForPresentationTime）谓词均含 should_stop 且 stop() 通知的
  正是同一 cv、队列满走丢弃不阻塞、EOF 后循环尾零阻塞、锁内无
  emit（stop/close 的 emit 都在锁外）；stop() 后置的 selectTrack
  （字幕）在已 join 的线程上走直接换装分支（ffmpeg_backend.cpp
  1440-1470），无二次 join。
  同族窗口路径另查出一处确定性顺序缺陷：窗口事件循环退出后
  `SDL_Quit()` 先于播放停止——解码/音频线程仍在跑时 SDL 音频子系统已拆。
  已修：`runPlayerWindow` 在循环后、SDL 拆台之前先 `player.stop()`
  （join 先于子系统拆台；空窗/无媒体两路 stop() 均有早退保护）。
  2026-10-15 收尾段缺陷定位与修复（巡检 R0233）：把 stop() 收尾段的
  全部等待面逐环过一遍后，定位到两处「无界且对中断不敏感」的等待——
  都是代码可证的缺陷，但触发条件本地都构造不出：

  1. **stop() 的 rewind seek 无界**：stop() 先把 should_stop_decoding_
     复位为 false，再做 rewind seek（seekToTimestamp(0)）。复位使中断
     回调不再 abort 这次 avio；而 stall watchdog 只在 av_read_frame 周围
     布防（network_read_started_ms_），rewind 的 seek 完全不在其覆盖内。
     若 demuxer 的 seek 会重新拉取 playlist/manifest（HLS/DASH 类），
     源已死时这次 seek 可以永久挂住，且它持着 decode_mutex_ 与调用方
     线程——正是「state=3 后主线程停在 stop()」的形态。
     修复：给 stop() 的 rewind 加 2s 预算（bounded_io_deadline_ms_ +
     kStopRewindLimit），中断回调在预算耗尽时 abort 这次 avio；seek 超时
     则失败返回，play() 另有自己的 rewind 路径，position 已清零。
  2. **playAudioFrame 的音频背压循环对中断不敏感**：
     `while (queuedBytes() > max_queued) SDL_Delay(5)` 的唯一出口是设备
     把队列排空。设备一旦停止排空（SDL 音频设备异常/被抢占），解码线程
     就停在 SDL_Delay 里，stop() 的 join 跟着挂死。
     修复：循环条件加 should_stop_decoding_ / seek_requested_ 谓词，退出
     后若队列仍满则丢弃该帧（seek 会清空设备队列，stop 不排空）。

  踪迹形态复核（修正本条此前的 trail 重建）：stderr 是逐行无缓冲
  （strace 证实每行一次 write(2)），故 incident 2 的 trail 完整有序；
  但 streaming 用例不传 --cache-dir，use_cache=false，DownloadProgress
  只从 avioReadCallback（缓存路径）发出——`download 100%` 不可能来自
  HLS master 用例，本条此前把同日志其他用例的 trail 拼进了 incident 2。
  HLS master 夹具不可 seek（media_info_.seekable=false），seek(1s) 立即
  失败；本地该 run ~0.2s 收尾，因为 main 抢在 EOF 前 stop() 杀掉解码循环。
  EOF 处理无歧义（playback_state_=Ended; emit(StateChanged); break;），
  故 state=3 坐实解码线程已退出、main 未到 pause() 的 emit——挂点确在
  stop() 内。

  复现尝试（如实）：本地累计 ~182 轮重放（142 轮既有 + 本轮 40 轮
  tsan 树三用例连跑，~2s/轮）零复现；gdb 强制竞态（main 停在 seek 行、
  解码线程跑）因 cov3 解码 ~0.15x 实时 + ALSA 开失败 ~100ms/帧，9s 内
  解码线程只到 1400ms/6037ms，未能逼出「Ended 后 seek」路径。两处缺陷
  的触发条件分别是「设备停止排空」与「rewind seek 做阻塞网络 I/O」，
  本地夹具（VOD ENDLIST，rewind 不做网络 I/O，strace 证实）都无法构造。
  结论：根因是代码可证的缺陷族，非本地可复现；SOAR_HANG_DUMP 探针
  保持布防，等下次 CI 挂死交栈以最终归因。
- **缓解**：watchdog 按设计把挂死转成干净断言失败（exit 124 带完整
  输出），套件不再整段超时；p2p 套件自定义 main 行缓冲 stdout
  （14226b2），下次挂死日志保底有 banner 与最后开始的 test case。
- **留栈探针（2026-10-10 落地）**：runCli 看门狗改 `timeout -s ABRT
  --kill-after=10`（timeout 仍退 124，重试逻辑不变）；soarAppMain 在
  `SOAR_HANG_DUMP=1` 时装 SIGABRT 处理器，打印当前线程 backtrace
  （带 tid 归属，进程向信号可能投递到任一线程）后按默认处置终止，
  runHeadlessOverHttpDir 为子进程设置该 env。下次 CI 挂死日志自带
  挂死线程的栈，可直接定位收尾段挂点。
  盲点补（9be541c，2026-10-14）：124 重试放行时首子的 dump 随被丢弃
  的捕获缓冲消失——单次挂死在绿腿日志里本会不可见。现两个流媒体
  helper 的重试先打印首子末 4096 字节（hangDumpTail 截尾，带单测）
  并 fflush 再重试；自此单次挂死也在绿腿日志留下栈，且即使重试子
  再挂死套件被 ctest 收割，已 flush 的尾巴不丢（p2p 行缓冲教训）。
- **残余（如实）**：两处收尾段缺陷已修（见上 2026-10-15 段），但根因
  未经本地复现坐实——触发条件（设备停止排空 / rewind seek 阻塞网络 I/O）
  本地夹具构造不出。SOAR_HANG_DUMP 探针保持布防，下次 CI 挂死交栈即可
  最终归因；若栈落在别处，按栈再查。
