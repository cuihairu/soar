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
  CI Windows 腿 PE 子系统/重定向捕获/窗口站住断言；nightly 安装器
  静默装→实跑→卸载走查。
- **残余（如实）**：`SDL_CreateWindow` 失败点（SDL_Init 成功后建窗
  失败，实践上极罕见）只打印不弹框不落日志——该路径无测试覆盖，
  为不突破覆盖率门禁未接线，留待后续测试收编批。
