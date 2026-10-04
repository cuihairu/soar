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
