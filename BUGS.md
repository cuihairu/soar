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
  - 七腿 CI 终证与覆盖率读数：见提交后的 run 记账（本条目随 run 落账）。
- **残余（如实）**：Windows 真机复测待用户——安装版双击进空窗、
  对话框选文件、资源管理器「打开方式 → Soar」关联播放三条链路
  （CI runner 无交互桌面，文件对话框与关联双击无法自动化）；
  Linux 文件管理器的实际打开链路（desktop MimeType 声明面已接线）
  未在真桌面环境走查。
