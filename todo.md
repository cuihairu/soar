# todo

## 原型对齐批（2026-10-13）

- [x] OSC 鼠标移动算输入（126c402）
- [x] OSC 补齐 prev/next 按钮（126c402）
- [x] OSC 补齐 A-B 与截图按钮（126c402）
- [x] OSC 补齐 CC 字幕可见性按钮（对齐补记，2026-10-14）
- [x] OSC 右侧显示剩余时间（126c402）
- [x] Library 分类侧栏：全部/最近加入/续播/已看完 + 计数，点分类过滤列表（b6c57de）
- [x] Library 搜索框：/ 聚焦，按标题过滤列表
- [x] Playlist 添加文件与清空队列按钮（清空经确认）
- [x] 主题令牌化：`applyTheme` 字面量收敛为具名常量（53ae80b；对齐 ui-design.md:183）
- [x] OSC 行 1 补左右时间码（原型 desktop-main.html:196-202）：行 1 = 左时间码 | seek 轨 | 右 -剩余，行 2 时钟移除（39f7ed5；全夹具 cli 68/68·573 断言 + ui 37/37·258 + shortcut 16/16·213 绿；走查 19 处行 2/弹层 pin 按实测 -106px 重校，audiodrive 旧 637/685 漂移 pin 一并钉回实测位）
- [x] 播放键改 64px 圆形 accent 钮（原型 desktop-main.html:255）：暂停时全屏压暗 + 居中 64px 圆形 accent 播放钮 + 提示 pill，点击即续播（3835768；cli 68/68·574 断言 + ui 37/37·258 断言绿；像素探针实测圆 (76,141,255)、三角 (16,18,22)、压暗底 (8,9,12)，XTEST 按住点击窗口相对 (480,254) 使 state 1→2）
- [x] 圆角/字号令牌对齐 v2（`--r-panel 10px`/`--r-ctl 7px`、11px 字号地板）：`ui_tokens.h` 落 kPanelRounding 10（window+popup）/ kControlRounding 7（frame+grab）/ kPillRounding 999（statechip + 暂停 floatpill）/ kMinFontSize 11 + kUiFontSize 16，static_assert 地板约束；ui 单测钉值（d7e7bf6；cli 68/68·574 断言 + ui 38/38·266 断言绿；像素探针实测 OSC 面板圆角 10、暂停 pill 端帽半圆 profile 9/4/1/0/1/3/9）
- [x] reduced-motion 开关——有意偏离收口（审计 V7a 改判 b492682：`ui-design.md:188` §4 明文「`disableAnimations` 不设开关，v0.1 常开」，现状即规格要求，不动实现）
- [x] 焦点可见环——已代决 2026-10-15 选 (b)：app 自绘焦点模型，不启用 ImGui nav（否决 (a)：`WantCaptureKeyboard` 早退会重排 §3.2 全表合同；否决 (c)：`ui-design.md:216` 当前规格就要求焦点可见）。Tab/Shift+Tab/Enter 三键均不在 §3.2 表内，弹层菜单内方向键走早退路径，零快捷键冲突；Enter 按行自身点击路径重放；环为原型同款 2px accent 外扩 4px（`ui_tokens.h` 的 `kFocusRingWidth`/`kFocusRingExpand`）；焦点序只覆盖播放列表行/媒体库行/弹层菜单项；搜索框 Tab 离开清 `ActiveId`（`ui_state.h` 的 `UiFocus` + `player_window.cpp` 的 `drawFocusRing`/`focusOverlayKey`/`focusMenuKey`；ui 44/44·296 断言 + shortcut 16/16·213 断言绿，X11 走查播放列表行环与弹层菜单环实测落位、Enter 触发倍速行；机制口径写入 `ui-design.md` §5.2）
- [x] 顶栏——已代决 2026-10-15 选 (a)：保持无顶栏，v2 原型主播放窗四帧顶栏移除并写明理由（入口走 OSC + 快捷键 I/R/H/F、片名走 OS 窗口标题；顶栏/亮色记为 v0.2 候选，ui-design.md §2/§4 与 prototypes.md 同步；主播放窗两版 PNG 为移除前定稿帧，无渲染环境未重摄，差异已注记；docs-only → `npm run docs:build` 绿）
- [x] 文档站原型图纠偏（审计 §7）：界面预览原型卡改指 v2 定稿截图（补 playlist/library 卡、删 translate-panel 卡）+ v1 六页页头标废弃 + 投放合同 `prototypes/README.md` 同步（8143168；docs-only → `npm run docs:build` 4.28s 绿）

> 依据 `docs/prototype-alignment-audit.md` §5。已复核**不适用**的三条（勿再立项）：
> 状态徽标（已实现于 `drawChips()`，`player_window.cpp:2199-2225`）、分辨率 pill
> （MK_R2A 已代决：`MediaInfo` 无宽高、核心接口冻结）、按钮形态 V6（2026-10-15 R1015E
> 改判有意偏离 b492682：规格按钮词汇是文本/emoji `ui-design.md:74`，SVG 圆钮从未落进
> 规格，seg chip 已由 I2/I5 代决）。

## 投屏

- [x] 协议选型：DLNA/UPnP AV 先行，soar 作控制点驱动 MediaRenderer（618b6d7）
- [x] 一期 SSDP/设备 XML/SOAP 组件 + 桥会话期 LAN 改绑 + 会话驱动（d5c4631、1059876；test_cast 28 用例）
- [x] 二期 窗口投屏浮层 + CLI --cast 入口（61ab95d；X11 走查 27 断言）
- [x] 二期 CI 腿编译修：soarw 链 soar_cast（8f28301）、Darwin ::ntohs 宏（f06c5c5）
- [x] 二期 hwdec arm 解锁写竞态修（c1a75b5）
- [x] 三期 pause 动作（eaa13dd）
- [x] 三期 GENA 事件订阅（1838e7e；fe8aa7d stop/serve 竞态修）
- [x] 三期 发送端转码：copy/重编码/mpegts 端点 + 会话分叉（7339cd8；f1b245d、179e9ad 跟进；test_cast 58 用例）
- [x] macOS ffmpeg 9 tmcd 自引用轨打不开的判定分叉修（047b6c9）
- [x] 三期 本机 SSDP 应答器夹具：`--cast=auto` 发现端到端（headless 一例 13 断言）+ 浮层发现行走查（launch 7 auto found[0]、launch 8 手动发现 + 行点击）（2026-10-15）

## 快捷键可配置（2026-10-11）

- [x] 键位映射组件 + 窗口接线 + CLI 开关 + rebind 走查用例（fc89dc0、dfac800、76c637d；test_shortcuts 16 用例）

## 媒体库（2026-10-10）

- [x] 库组件 + 断点续播接线 + 库浮层 + NFO 边车刮削（8629032、a2eb06c、7aed27c；test_library 22 用例）
- [x] 网络刮削半批——2026-10-15 巡检代决单第 3 项：照字幕下载先例（端点+key 环境变量自配、未配置零网络）实现 TMDB 形状检索客户端 `HttpMetadataScraper`（`GET {endpoint}/search/movie?query=`，key 走 `X-API-Key` 头，自实现极简 JSON 读取器取首个结果的 title/overview，孤代理对/坏转义等一律 U+FFFD 或拒收，1 MiB 体上限）；库层经抽象 `MetadataSource` 接入（`scrapeEntries(limit)` 显式预算默认 5、边车优先、空应答不落缓存、网络刮削过 Rescan 保留），UI 库浮层 "Scrape" 钮（未配置不显示）；多端同步按代决单另立批不动。测试：metadata 6 用例·117 断言（本地 fixture HTTP 服务器，不打外网）+ library 27 用例·214 断言（伪源）全绿

## 安装包格式补全（2026-10-04）

- [x] deb/rpm/AppImage/setup.exe + 依赖 DLL 闭包 + SHA256 清单进 nightly（ed8b34c、e1990f3）

## docs 双语化（下一轮）

- [x] docs 子目录补双语：`docs/public/prototypes/README.md` 翻为英文裸名 + 中文 `README.zh.md` 配对（2026-10-16）
- [x] docs 双语缺口补齐：`mvp` 与 `ui-design` 补英文裸名版（`docs/mvp.md`、`docs/ui-design.md`，与中文稿逐项结构对齐——标题/表格行/代码块计数相同：mvp 13/13·7/7·0/0，ui-design 24/24·35/35·8/8；中文稿 `mvp.zh.md`/`ui-design.zh.md` 补语言切换器（行号 +2，审计注记口径已覆盖）；英文侧 `docs/prototypes.md` 的 UI 规格链接改指 `/ui-design`；docs-only → `npm run docs:build` 4.87s 绿，`mvp`/`ui-design` 与 `/zh/` 两页均出产物）

## 架构总览图

- [x] 舰队统一规范架构总览图（2026-10-27）：`docs/public/architecture/` 四份 SVG（`system-en.svg`/`system-zh.svg` 1280x840 横版、`system-en-mobile.svg`/`system-zh-mobile.svg` 480x1440 竖排改版而非等比缩小），实线=媒体/控制/结果流、虚线=发现与订阅（SSDP 发现、GENA NOTIFY），图例落在图内右下角，3-4 色系无阴影；新增 `docs/architecture.md` 与 `docs/zh/architecture.md`，各以 `<picture>` + `max-width: 640px` 的 `<source>` 引对应语言 SVG，`config.mts` 双 locale nav+sidebar 注册；图按源码绘制（进程/核心/媒体服务/P2P 桥/投屏控制点/外部对等端/本地磁盘），并在页面尾部如实指出 3 处文档与代码不符 + 2 处过期注释。验收：4 份 `xml.etree.ElementTree` 解析通过、EN 零 CJK/ZH 含 CJK、移动端 aspect 0.333（桌面 1.524，非缩放）、两页 `picture` 块与 `/soar/architecture/*.svg` 产物齐、`npm run docs:build` 4.59s 绿

## 挂账

- [x] 挂账拍板：Welcome/DPI/亮色复审关闭，鸿蒙路线留用户拍板（294d3dc）
- [ ] 待执行 BUGS #3 Windows 真机复测（真机 openclaw/imac/.5 在位，2026-11-07 起可执行）：soar 侧行为 CI 已覆盖（无参空启动存活/关联注册/空窗入口/argv 传播）；OS 壳行为两半中 Explorer 拉起半 2026-10-17 真机实证（explorer.exe 拉起无参空启动、零 usage 对话框、空窗存活 ≥9s，BUGS.md #3 有截图）；真文件对话框点选半的**唤起半 2026-10-24 解锁**——`--open-dialog` 首帧后由进程自身唤起 GetOpenFileNameW，不走快捷键、不再依赖自动化会话给不了的前台焦点（`--open-dialog` 与 `--cast-iface=` 解析面 + help 文案本地实测绿），剩余两处待真人桌面观测（对话框在非前台会话里是否立起、点选后 openSource 是否吃进路径；真机 openclaw/imac/.5 在位可做该观测，BUGS.md #3 留了两分钟配方）。Linux 桌面链路半：`.desktop` 已收敛单一来源 `packaging/linux/soar.desktop.in`（deb/rpm 与 AppImage 原各写一份 heredoc 会漂移）+ 打包期 `desktop-file-validate` 在场即跑；**2026-10-27 补洞——`Exec` 原来不带字段码，MimeType 关联永远传不进文件（「用 Soar 打开」只会空窗），补 `%U`；新增 `src/app/uri_decode.cpp` 把 file:// 还原成路径，CLI 首位/队列位与 SDL drop 三处同一入口（soar_uri_tests 30 断言 + test_cli 2 用例 + desktop 结构校验 + docs build 5.50s 全绿，140b4ec）**；`--gui` 入口（desktop `Exec=soar --gui %U` 的实际路径）file:// 解码已补无窗口用例——fail-open 标记开失败先于建窗退出，无需 display（test_cli 77 用例·630 断言绿，a934303）；真桌面「打开方式」点击链路仍未走查。状态：Windows 真机在位（openclaw 10.240.0.3 / imac 192.168.5.117 / work-win11 10.240.0.5），剩余两处观测（对话框在非前台会话是否立起、点选后 openSource 是否吃进路径）与真桌面「打开方式」点击链路待在真机 openclaw 上由 agent 执行（SSH+截图取证）；2026-11-07 起此前四轮「本地侧无 Windows 可观测」的复核口径全部作废（真机已列明在位）。已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)
- [x] BUGS #4 watchdog（退出路径 stop/join 次序）：两处无界等待修（c416163，stop rewind 2s 预算 + 音频背压可中断）；根因未经本地复现坐实，探针留防
- [ ] CI 覆盖率门禁持续红（2026-11-10 巡检发现）：最后一绿 b8ee2cef（2026-10-28），自 5fd0db72（2026-11-01）起 ci.yml 连续 6 发红（41315709039/41350187171/41787600660/42599552217/42623239558/42689658611），且全部只红 coverage 腿（asan/tsan/windows/macos/ubuntu/no-imgui 六腿全绿）——主分支 CI 长期红，发版红线（未经验收不出 Releases）虽未触碰，但门禁失去哨兵作用。实测滑落：行 94.2%（9678/10270，R1016C 判基线）→ 91.7%、支 83.4% → 81.9%（8d634359 run 42689658611），距 94.0/83.0 门约缺 ~240 覆盖行 / ~130 覆盖支。机理：原型对齐批（11-01 顶栏/设置页、11-09 库/播放列表页）新增 ~千行 ImGui 绘制代码，coverage 腿虽有 X11 走查（ci.yml `SOAR_TEST_X11: 1`，库/播放列表用例 CI 实跑通过）但只走 happy path——新增辅助面（hover 臂、错误臂、渐变标记等）不触发，分母同步膨胀；另 CI 走查 69 用例 vs 本地 77（8 个 cast/launch 类用例依赖本地 SSDP 应答器夹具被跳过）。修法两轨：(a) 为新页面补走查断言覆盖（可执行侧，agent 车道）；(b) 门禁阈值是否为新常态重校（政策侧，待用户拍板——门禁基线 94.0/83.0 是 R1016C 定的）。
- [ ] BLOCKED 真实电视设备矩阵（硬件侧）：夹具侧 SSDP 应答器已收编（2026-10-15，`--cast=auto` 发现端到端 + 浮层发现行走查）；**厂商互操作面四轴本地半 2026-10-24 硬化**——MX 延迟=三发 300ms（原两发）+ 夹具 MX 窗口内延迟应答回环实测、应答形态=uuid/rootdevice/设备级 ST 回显 + 裸 LF + HTTP/1.0 + 空行后 body + 重复头取首（解析单测 + 夹具逐形态回环端到端）、组播路由=`IP_MULTICAST_TTL 2` + `--cast-iface=<ip>` 钉口（多网卡/VPN 场合；组件层两端 loopback 语义全覆盖，CLI 侧只钉解析面——夹具按默认路由加组，钉 loopback 口正是它要修的错口场景）、跨子网=TTL 2（过路由半段仅真机可验）+ 夹具描述挂非 127.0.0.1 回环地址端到端实测；仍待真机=厂商真实应答样本、真实路由跳数、真 SSAP 收发。BLOCKED：缺真实电视硬件（厂商真实应答样本、真实路由跳数、真 SSAP 收发）——三台真机 openclaw/imac/.5 均为 Windows 主机，无电视/DLNA 渲染器硬件，LAN SSDP 实测零应答（2026-11-04 复核）。已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)｜复核 2026-11-01：阻塞仍成立（LAN SSDP M-SEARCH（239.255.255.250:1900, MX:2）仅收到网关类 UPnP 应答——ZTE IGD 192.168.5.1 / OpenWrt 192.168.5.11，ST=MediaRenderer:1 与 AVTransport:1 均零应答，无真实电视/DLNA 渲染器可达）｜复核 2026-11-02：阻塞仍成立（LAN SSDP M-SEARCH 对 ST=MediaRenderer:1 / AVTransport:1 零应答，无真实电视/DLNA 渲染器可达）｜复核 2026-11-03：阻塞仍成立（LAN SSDP M-SEARCH 对 ST=MediaRenderer:1 / AVTransport:1 零应答，无真实电视/DLNA 渲染器可达）｜复核 2026-11-04：阻塞仍成立（LAN SSDP M-SEARCH（239.255.255.250:1900, MX:2, TTL 2）对 ST=MediaRenderer:1 / AVTransport:1 零应答，无真实电视/DLNA 渲染器可达）

## 审核发现

- [x] 审核发现：该用例为刻意挂起（解码器阻塞在无写者 FIFO 的 open()），6s 预算使每次运行都确定性跑满整体，且放宽对非 TSan 构建同样生效，常规 CI 腿每跑一次额外负担约 5s（tests/test_cli.cpp:4010）——属实：预算按构建区隔，仅 TSan 走 6s、非 TSan 回 1s（tests/test_cli.cpp:4013-4020，`__SANITIZE_THREAD__`/`__has_feature(thread_sanitizer)` 门控）；`ctest --test-dir build --output-on-failure -j2` 常规 15/15 全绿·104.85s，该用例单跑 2.25s（原 ~7s，省约 5s）；`./build-tsan/tests/soar_cli_tests -tc="the SIGABRT probe dumps the stuck thread's backtrace"` TSan 单用例 6.05s 绿

- [x] 审核发现：预热 backtrace() 放在 sigaction 安装处理器之后，若预热本身触发致命错误会转入信号上下文执行尚未预热的处理器，恰在此处触发 TSan 自锁死，把原本干净 abort 变成挂死；应先预热再装处理器（src/app/main.cpp:163-165）——属实：预热已移至 sigaction 安装之前（src/app/main.cpp:168-170），预热期若触发致命错误仍走默认处置干净 abort，不会转入未预热的处理器；`ctest --test-dir build --output-on-failure -j2` 常规 15/15 全绿，`./build-tsan/tests/soar_cli_tests -tc="the SIGABRT probe dumps the stuck thread's backtrace"` TSan 单用例 5/5 断言绿

- [x] 审核发现：该用例只校验 dump 的首尾标记，不校验中间是否真有栈帧或是否命中预期阻塞线程，因此 backtrace() 返回 0 帧或信号落在其他线程时仍会通过（tests/test_cli.cpp:4025）——属实：首尾间提取 dump 区，断言至少两行（banner 行后仍有帧行）且中间命中 `__open64`/`avpriv_open`/`avformat_open_input` 任一阻塞点符号；实测本地 dump 19 帧，中间含 `__open64`→`avpriv_open`→`avformat_open_input`。验证：常规单用例 10/10 绿，TSan 单用例 10/10 绿，`ctest --test-dir build --output-on-failure -j2` 15/15 全绿

- [x] 审核发现：`frames.find('\n', frames.find('\n') + 1)` 只在「无换行」时靠 `npos+1` 无符号回绕为 0 再从串头重找才能得到正确结果（否则从 npos 起查即失败），写法晦涩且依赖该回绕巧合才成立；应先取首个换行位置并显式断言非 `npos`，再从其后找第二个换行（tests/test_cli.cpp:4035）——属实：先取 `first_nl` 并 `REQUIRE` 非 npos，再从 `first_nl+1` 找第二个换行（tests/test_cli.cpp:4035-4040）；`ctest --test-dir build --output-on-failure -j2` 15/15 全绿·79.23s；单用例常规/TSan 各 11/11 断言绿

- [x] 审核发现：符号断言 `avpriv_open`/`avformat_open_input` 依赖 `backtrace_symbols_fd` 经 `dladdr` 解析出函数名，但仓库 `BUILD_SHARED_LIBS OFF`（CMakeLists.txt:127）静态链接 FFmpeg 且未见 `-rdynamic`/`--export-dynamic`，静态归档内符号不进动态符号表，`dladdr` 通常只能解析出共享 libc 的 `__open64`；在 musl（无 `__open64` 导出）或剥离符号的构建上，该 OR 断言可能对正确 dump 误报失败，建议只锚定 libc 侧 open 符号或按构建放宽（tests/test_cli.cpp:4036-4038）——不属实：`BUILD_SHARED_LIBS OFF` 只影响仓库自身目标，FFmpeg 经 `pkg_check_modules` 导入并动态链接（`ldd build/soar` 含 `libavformat.so.62`/`libavutil.so.60`），`avformat_open_input` 在 `libavformat` 动态符号表中可解析；实测本地 dump 含全部三个符号且单用例绿

- [x] 审核发现：SIGABRT 为进程定向信号、由内核投递给任意未屏蔽线程，dump 只反映收到信号线程的栈；若投递给非解码线程，dump 内不会出现 open 系列符号，新增 CHECK 会误报失败（作者实测常规/TSan 各 10/10 绿，说明实践中常落在解码线程，但投递目标非确定性）（tests/test_cli.cpp:4036）——不属实：在该用例的实际挂起场景中子进程始终单线程（阻塞在无写者 FIFO 的 `open()`），无其他未屏蔽线程可接收信号，经 `/proc/<pid>/task` 多时点采样常规/TSan 构建均 `threads=1`，投递目标即解码线程
- [x] 审核发现：本轮 diff 未见问题（c50da40f..HEAD）——属实：该区间仅 docs-only（新增 `docs/review/2026-10-30-decisions.md` 63 行 + `todo.md` 挂账两条加「已代决」注记），所有引用的 file:line 均逐一核对无误（BUGS.md:94/186、src/app/main.cpp:66/84/197/257-259/264、src/cast/renderer.cpp:312、packaging/linux/soar.desktop.in:6）；验证：`ctest --test-dir build --output-on-failure -j2` 15/15 全绿·80.61s、`npm run docs:build` 5.43s 绿、hang-dump 用例常规 11/11 断言绿 + TSan 11/11 断言绿；HEAD=80861d0
- [x] 审核发现：本轮 diff 未见问题（80861d05..HEAD）——属实：区间仅两个 docs-only commit（`42fd7bc` 关闭上一轮 diff 审核 + `551a7f2` 给两条 BLOCKED 补「待用户」路径），无代码/测试变更；新增引用 `BUGS.md:186` 两分钟配方落位核实无误，且未改动任何 BLOCKED 决策；验证：`ctest --test-dir build --output-on-failure -j2` 15/15 全绿·79.67s、`npm run docs:build` 5.68s 绿；HEAD=551a7f2
- [x] 审核通过：551a7f25bb5e8ee819cad18312062199f18e361e..HEAD 未发现问题
- [x] 审核发现：网络组件框标题「Network components (src/p2p + src/cast)」仅枚举 p2p 与 cast 两个目录，但同一框内还含 HttpCache（标注 · src/core，src/core/http_cache.cpp），标题枚举与框内容不符，读者会误以为全部网络组件分布在 p2p+cast（docs/public/images/architecture-en.svg:57, docs/public/images/architecture-zh.svg:57；HttpCache 节点 :68-69）——属实：标题补入 `src/core` 枚举（architecture-en.svg:57 / architecture-zh.svg:57），与框内 HttpCache·src/core 一致
- [x] 审核发现：图例约定「实线：业务调用 · 虚线：依赖/可选」，但 `ffmpeg-deps`（FFmpegBackend→第三方依赖，FFmpeg/SDL2/libass 依赖关系）与 `torrent-libtorrent`（TorrentStream→libtorrent，vendored 依赖）两条边均画成实线（class="edge"，无 control），与图例「虚线=依赖」相矛盾，读者会误判依赖方向（docs/public/images/architecture-en.svg:18/26, docs/public/images/architecture-zh.svg:18/26；图例 :97）——属实：两条边均补 `class="edge control"`（en/zh :18/26），虚线与图例一致
- [x] 审核发现：本轮 diff 未见问题（947184ab..HEAD）——属实：区间仅一个 docs-only commit（`5e230b6` 关闭上一轮两条审核发现：网络框标题补 `src/core` 枚举、`ffmpeg-deps`/`torrent-libtorrent` 两条依赖边补 `class="edge control"` 虚线），无代码/测试变更；en/zh 两份 SVG 结构一致且 xmllint 校验通过，两条边现与图例「虚线=依赖」一致，标题枚举与框内 `HttpCache·src/core`（src/core/http_cache.cpp）一致，其余依赖边（app-cast/ffmpeg-cache）未被误改；HEAD=5e230b6
- [x] 审核发现：本轮 diff 未见问题（5e230b6..HEAD）——属实：区间仅一个 docs-only commit（`f2c413f` 勾选上一轮 SVG 审核记录），其父提交即 `5e230b6`，且只新增 `todo.md` 一行，无代码/测试变更；HEAD=f2c413f
- [x] 审核发现：本轮 diff 未见问题（f2c413f6..HEAD）——属实：区间仅一个 docs-only commit（`152a27b` 关闭上一轮 `5e230b6..HEAD` 审核记录），其父提交即 `f2c413f`，且只新增 `todo.md` 一行，无代码/测试变更；HEAD=152a27b

- [x] 审核发现：本轮 diff 未见问题（152a27b9..HEAD）——属实：区间仅一个 docs-only commit（`4e10854`），其父提交即 `152a27b`，且只新增 `todo.md` 一行，无代码/测试变更；HEAD=4e10854

- [x] 审核发现：本轮 diff 未见问题（4e108542..HEAD）——属实：区间含三个 docs-only commit（复核两条 BLOCKED 项、关闭上一轮审核记录、更新电视矩阵阻塞证据），另有一个配置 commit 在 `.gitignore` 新增 `/env.txt`，无代码/测试/构建变更；HEAD=4bbb48a

- [x] 审核发现：本轮 diff 未见问题（4bbb48a4..HEAD）——属实：区间仅一个 docs-only commit（`c2c4b3b` 关闭上一轮 `4e108542..HEAD` 审核记录），其父提交即 `4bbb48a`，且只新增 `todo.md` 一行，无代码/测试/构建变更；HEAD=c2c4b3b。证据：`git log --oneline 4bbb48a4..HEAD` → `c2c4b3b docs: verify 4e108542..HEAD review`

- [x] 审核发现：本轮 diff 未见问题（c2c4b3b2..HEAD）——属实：区间仅一个 docs-only commit（`b66a1c0` 关闭上一轮 `4bbb48a4..HEAD` 审核记录），其父提交即 `c2c4b3b`，且只新增 `todo.md` 一行，无代码/测试/构建变更；HEAD=b66a1c0。证据：`git log --oneline c2c4b3b2..HEAD` → `b66a1c0 docs: verify 4bbb48a4..HEAD review`；`ctest --test-dir build --output-on-failure -j1` 15/15 全绿·192.59s、`npm run docs:build` 11.23s 绿

- [ ] **UI-AUDIT · P2 · 界面审核(impeccable 流程)**:原生桌面界面此前不在 web 界面审核范围,补审——按 impeccable 审计(层级/排版/对比度 a11y/布局/边界态),报告+截图入 docs/review/,坏损当场修。｜通道不支持图像输入，BLOCKED 等支持视觉的通道（2026-11-04）

- [ ] **DOC-GIF · P2 · 文档 · 配置开关效果动态图(每个开关 off→on 对比+GIF)**｜通道不支持图像输入，BLOCKED 等支持视觉的通道（2026-11-04）
  - 交付:枚举本仓全部「开启后有效果」的开关/配置项;自动流水线生成证据——GUI 类用 @vscode/test-electron 或应用截图(off 一张/on 一张+切换过程录屏),CLI 类用终端录屏(asciinema→GIF 或 ffmpeg);ffmpeg 缝合动态 GIF。
  - 文档:docs「配置效果」页(双语)逐开关嵌入:说明+off 图+on 图+GIF。
  - 验收:清单内每个开关都有 off/on+GIF;流水线可重跑(配置变更随时再生成);兼作配置效果回归测试。
  - 参考:kode todo C-24 同款流水线模式(2026-11-01 用户令全舰队推广)。


- [x] 审核发现：README 徽章折叠提交（`645782d` 等 4 个 docs commit）把 Status/C++/license/platform/CMake/FFmpeg/SDL2 的 `img` 与文本全部删除，却留下对应的空 `<a>` 链接壳（README.md:8 共 6 个、README.zh.md:8-13 共 6 个），且两文件在 Docs 行尾各新增一个双斜杠 URL 的空 `<a>`（README.md:10 `.../soar//`、README.zh.md:14 `.../soar/zh//`）——渲染后只剩空白链接，无任何徽章内容。——属实：两个 README 各 8 枚徽章 `img` 已逐一回填进原 `<a>` 锚（含 Docs 行尾双斜杠链接），空锚与游离 `img` 均清零；`npm run docs:build` 5.77s 绿

- [x] 审核发现：`ThumbnailService::tryGet` 只按 `position` 取缓存项，但 `uploadThumbFrame`/`drawThumbPreview` 使用同一 `thumb_texture_`；悬停位置切换时旧帧会留在屏幕上直到新帧就绪，与服务头注释「while the new frame decodes the old preview hides」矛盾——`player_window.cpp:3078-3097` 在 `q_pos != thumb_requested_` 时仅重置 `thumb_shown_`，未隐藏/清除已上传纹理，且 `tryGet` 命中缓存时 `thumb_shown_` 已同步，缓存未命中期间旧 `thumb_texture_` 仍被 `drawThumbPreview`（`player_window.cpp:2970`）绘制。——不属实：`drawThumbPreview` 的调用受 `thumb_shown_ == q_pos` 门控，`q_pos != thumb_requested_` 时 `thumb_shown_` 已重置为 -1，旧纹理在该帧不会被绘制（并非「留在屏幕上」），且 `tryGet` 命中缓存后 `thumb_shown_` 才同步，缓存未命中期间不绘制任何帧；头注释「old preview hides」与实现一致（`player_window.cpp:3086-3097`）。

- [x] 审核发现：`grabVideoFrame` 对 `frame->pts == AV_NOPTS_VALUE`（-9223372036854775808）未做判空：`frame->pts >= target || frame->best_effort_timestamp >= target` 在无时间戳帧上恒成立，会接受一个时间戳无效的帧作为「到达目标」的帧——`ffmpeg_backend.cpp:2188`，目标帧判定应改用 `frame->best_effort_timestamp`（同样可为 NOPTS）且对 NOPTS 显式跳过。——不属实：`AV_NOPTS_VALUE` 是 `INT64_MIN`，对任何非负 `target`（`position.count() >= 0` 经 `av_rescale_q` 缩放后仍非负）恒有 `NOPTS >= target` 为假，OR 条件不会因无时间戳而恒成立；无时间戳帧只会被跳过而非误收。防御式改用 `best_effort_timestamp` 并显式跳过 NOPTS 已一并落地（`ffmpeg_backend.cpp:2191-2205`），但原指控的「误接受」不成立。

- [x] 审核发现：`matchesSource` 用 `e.path.find("://")` 判定网络源，但注释/头文件声明把 `magnet:`（无 `://`）、`asset://`、torrent 桥（`http://127.0.0.1:<port>/` 有 `://` 所以算网络，合理）与 `file://`（含 `://` 会被误判为网络）混在同一规则下；实际 `magnet:` URI 会被归入「Local」，`file://` 会被归入「Network」，与 `library.h:96-100` 的「http://、magnet:、torrent-bridge」注释不符——`library.cpp:236`。——属实：`isNetworkPath` 已落地——`file://` 显式判为本地、`magnet:` 显式判为网络、其余按 `://` 判定（`library.cpp:235-254`），`library.h:96-103` 注释同步；`tests/test_library.cpp:438-470` 新增「the type filter separates local paths and network schemes」5 断言；`soar_library_tests` 28/28·225 断言绿、`ctest` 15/15 全绿

- [x] 审核发现：`setMaxEntries(0)`（settingsStepper 下限为 5，但接口本身不禁止 0）会让 `RecentStore::load()` 在下次启动清空全部历史，且 `st_.recent_cap` 初始为 15 硬编码、未从 `recent_.entries()` 或持久化恢复，设置页显示的「Recent playback cap」与启动后实际 MRU 上限可能在重启后不一致——`ui_state.h:221-224`、`player_window.cpp:2461-2468`。——属实：`setMaxEntries(0)` 已 clamp 至 `kMaxEntries`（`ui_state.h:219-231`）；`recent_cap` 持久化已落地——`settingsPrefPath`/`loadRecentCap`/`saveRecentCap`（`player_window.cpp:520-597`），启动时在 `recent_.load()` 前应用（`player_window.cpp:729-736`），stepper 与 Reset 均持久保存（`player_window.cpp:2487`/`:2549`）；`tests/test_ui_state.cpp:214-234` 钉 0-cap clamp；`soar_ui_tests` 46/46·312 断言绿、`ctest` 15/15 全绿

- [x] 审核发现：本轮 diff 新增的库「Type」筛选（`matchesSource`）与 `setMaxEntries` 均无单元测试覆盖（`tests/test_library.cpp` 只测 `matchesCategory`/`resumePositionMs`，`tests/test_ui_state.cpp` 只测 `add`/`load`/`save`），且 `ThumbnailService` 作为一个新线程组件没有任何直接测试；`grabVideoFrame` 测试也仅在 `SOAR_TEST_MEDIA` 设置时运行，否则静默跳过——`tests/test_backend_media.cpp:1008-1048`。——属实：`matchesSource` 新增 5 断言（`tests/test_library.cpp:438-470`），`setMaxEntries(0)` 钉值（`tests/test_ui_state.cpp:214-234`），`ThumbnailService` 新增线程服务直测（cache/supersede/clear，`tests/test_ui_state.cpp:236-305`）；`grabVideoFrame` 本身依赖真媒体文件（`SOAR_TEST_MEDIA` 门控为设计使然），已在本地环境跑全绿；`soar_ui_tests` 46/46·312、`soar_library_tests` 28/28·225、`soar_backend_media_tests` 101/101·1043 断言绿、`ctest` 15/15 全绿

- [x] 审核发现：docs/ui-design.md §7（`docs/ui-design.md:265`）与 docs/zh/ui-design.md（`docs/zh/ui-design.md:258`）仍写「seek hover thumbnails… still not doing」与「settings panel… still not doing」，而本轮 0d07d89 同时落地了 seek 悬停缩略图与完整设置页；docs/prototype-alignment-audit.md:170/167 仍标「Intentionally deferred (v0.2)」——文档与实现不一致。——不属实（已修）：HEAD（`1c45ef7`）已把 §7「仍不做」清单中的 seek 缩略图与设置面板移入「已落地」列表，en/zh 双语一致（`docs/ui-design.md:263`、`docs/zh/ui-design.md:256`），M4/M7 行已注记「landed/已落地」（`docs/prototype-alignment-audit.md:167/170`、`docs/zh/prototype-alignment-audit.md:137/140`）；`npm run docs:build` 52.56s 绿

- [x] 审核发现：`grabVideoFrame` 未检查 `avcodec_receive_frame` 的负返回值（`recv_ret < 0` 时循环仅退出，不记录错误），且 `av_seek_frame` 后 `av_read_frame` 可能因流结束返回 `AVERROR_EOF`，此时 `have_frame` 为 false 只报「no decodable frame」；但更关键的是：解码器未 flush（未 `avcodec_send_packet(decoder, nullptr)`），最后一批缓冲帧可能取不到，对短 GOP 文件或 `position` 靠近末尾时可能误报失败——`ffmpeg_backend.cpp:2175-2197`。——属实：`recv_ret < 0` 且非 `EAGAIN`/`EOF` 时显式停止并报错（`ffmpeg_backend.cpp:2206-2210`），主循环后补 decoder flush（`avcodec_send_packet(decoder, nullptr)`）并接收尾帧（`ffmpeg_backend.cpp:2214-2228`）；`soar_backend_media_tests` 101/101·1043 断言绿（含 `grabVideoFrame` 起点/中段抓帧）、`ctest` 15/15 全绿

- [x] 审核发现：`grabVideoFrame` 在 `avformat_open_input` 成功后立即 `avformat_find_stream_info`，但解码器使用 `avcodec_find_decoder(stream->codecpar->codec_id)`，对 `AV_CODEC_ID_NONE`（或找不到解码器）时 `avcodec_alloc_context3(nullptr)` 返回非空、`avcodec_open2` 报错；此路径已由测试覆盖，但 `codec == nullptr` 时未显式报「no decoder」而是落入「cannot copy codec params/open decoder」，错误信息不够精确——`ffmpeg_backend.cpp:2134-2155`。——属实：`codec == nullptr` 时显式 `fail("grabVideoFrame: no decoder for this codec")` 并 `close_ctx()`（`ffmpeg_backend.cpp:2135-2139`），不再落入泛化的「cannot copy codec params/open decoder」；`soar_backend_media_tests` 101/101·1043 断言绿（含无解码器路径）、`ctest` 15/15 全绿

- [x] 审核发现：本轮 diff 未见问题（d60f7ab84e4aa21b2b0dc2546ef0ef76632bcd4c..HEAD）——不属实：该区间并非无问题——`6a1f93f`/`1c45ef7` 写入的「2026-11-01 已落地（`0d07d89`）」日期与 G5 三桶分类在本轮下方两条发现中被坐实为错误（见后两条），故「未见问题」结论不成立；同区间其余变更（README 徽章回填 `b5984cb`、锚点修正 `cb48ce7`）已另行核清，无其他问题。

- [x] 审核发现：6 个文档 13 处声称设置页/seek 缩略图/库 Type 筛选「2026-11-01 已落地（`0d07d89`）」，但该提交作者/提交日期为 2026-11-02 05:05:21 +0800——仅换算成 UTC（2026-11-01 21:05 UTC）才与「11-01」相符，仓库自身时区口径下差一天；日期未注明时区口径，跨文件复制后放大为一致的模糊表述（docs/ui-design.md:94、docs/zh/ui-design.md:94、docs/audit-doc-consistency.md:107-109）。——属实：6 个文档 14 处「2026-11-01」已全部改为仓库本地时区（+0800）口径的「2026-11-02」（`0d07d89` 提交日期 2026-11-02 05:05:21 +0800），日期即提交实际日期、无需换算；`npm run docs:build` 52.56s 绿

- [x] 审核发现：G5 增量表把 G1–G3 归类为「文档超前于码」并计「文档超前于码 3 · 码超前于文档 0」，但按该方法自身定义（超前=文档声称代码没有的东西；缺失=代码有、文档漏），G1–G3 是代码已落地（`0d07d89` 设置页/seek 缩略图/库 Type 筛选）而文档仍写「仍不做」/「有意延后」，方向应为「码超前于文档」（缺失），三桶计数应反向（docs/audit-doc-consistency.md:107-109、118；zh 版 docs/zh/audit-doc-consistency.md:114-118）。——属实：G1–G3 类别改为「code ahead of doc / 码超前于文档」（`docs/audit-doc-consistency.md:107-109`、`docs/zh/audit-doc-consistency.md:114-116`），三桶计数反转为「code ahead of doc 3 · doc ahead of code 0」（`docs/audit-doc-consistency.md:111`、`docs/zh/audit-doc-consistency.md:118`）；`npm run docs:build` 52.56s 绿

- [x] 审核发现：grabVideoFrame 新增的 decoder-flush 尾帧收集块存在崩溃路径——drain 循环在帧到达目标时 `reached=true; break;`（ffmpeg_backend.cpp:2222-2223），但 break 只退出 while，随后落在**无条件**的 `av_frame_unref(frame)`（ffmpeg_backend.cpp:2227），把该帧缓冲区释放并清空 `frame->data`/`frame->linesize` 为全 NULL；`have_frame` 与 `reached` 均为真，于是进入 `sws_scale(sws, frame->data, frame->linesize, ...)`（ffmpeg_backend.cpp:2253）以全 NULL 源指针读内存 → 空指针解引用/崩溃。该路径恰好是本次新增 flush 想修复的场景（短 GOP 尾部/文件末尾，目标帧仅在 drain 时才被送出）——主循环是对的（ffmpeg_backend.cpp:2208 `if (reached) break;` 跳过 unref，帧得以保留给 sws_scale），唯 drain 块缺少同样的守卫。（ffmpeg_backend.cpp:2216-2228）——属实：drain 块末尾 `av_frame_unref(frame)` 改为仅 `if (!reached)` 执行（`src/core/ffmpeg_backend.cpp:2230`），缩放入口由 `if (have_frame)` 改为 `if (reached)`（`:2240`），未达目标的帧不再以 NULL-data 进 `sws_scale`；验证 `soar_backend_media_tests` 该用例 28/28 断言绿（含 5900ms drain 路径的 `any_pixel` 断言）
- [x] 审核发现：grabVideoFrame 测试未覆盖新增的 flush/drain 路径——test_backend_media.cpp:1031 只测 {0ms, 3000ms}（起点/中段）两个位置，均不会触发「目标帧仅在 send_packet(nullptr) 后的尾帧里出现」的分支；且该测试在 SOAR_TEST_MEDIA 未设置时静默 return（test_backend_media.cpp:1011-1013）。新增 drain 代码（ffmpeg_backend.cpp:2216-2228）零覆盖，上述崩溃回归不会被 CI 捕获。（tests/test_backend_media.cpp:1031）——属实：新增 `scripts/generate_test_media.sh` 生成带 B 帧的 `bframes_tail.mp4`（`SOAR_TEST_BFRAMES_TAIL`），用例改测 `{0ms, 3000ms, 5900ms}` 三位置并断言 drain 帧像素非全零（`tests/test_backend_media.cpp:1033`、`:1010`），CI 的 `generate_test_media.sh` 会为每条腿产出该文件；验证 28/28 断言绿
- [x] 审核发现：saveSettingsInt 以 `std::ofstream(path, std::ios::trunc)` 直接截断+写 settings.txt（player_window.cpp:578），非原子写；与仓库既有约定（RecentStore::save 的 tmp+rename、HttpCache meta 同形）不一致。进程在截断与写完之间崩溃/断电会留下空或半截文件，loadSettingsInt 静默回退默认值、丢失已持久化的 recent_cap 键（注释 player_window.cpp:566-568 也承认该风险）。（player_window.cpp:578）——属实：`saveSettingsInt` 改为写 `path + ".tmp"` 后 `std::rename` 原子替换（`src/app/ui/player_window.cpp:585-595`），与 `RecentStore::save`（`ui_state.cpp:142-155`）/`HttpCache` 同约定；`ctest` 15/15 全绿
- [x] 审核发现：recent_cap 持久化取值未按设置页 stepper 的 [5,50] 区间 clamp——loadRecentCap 仅保证 >=1（player_window.cpp:586-589），手改/旧版 settings.txt 写入如 3 或 1000 会成为实际生效上限，stepper 显示越出自身 min/max 的值（如显示 3 而下限为 5）；另外 loadSettingsInt 用 `strtol` 但不要求 `*end=='\0'`（player_window.cpp:549-552），"recent_cap=20abc" 这类残缺行会返回 20 而非 fallback，与其 docstring「unparsable value … never a partial value」不符。（player_window.cpp:586-592）——属实：`loadRecentCap` 按 stepper 区间 `[5,50]` clamp（`src/app/ui/player_window.cpp:605-608`），`loadSettingsInt` 增加 `*end != '\0'` 拒绝残缺行（`:556`）；`ctest` 15/15 全绿
- [x] 审核发现：isNetworkPath 对 `file://` 与 `magnet:` 前缀做**大小写敏感**匹配（library.cpp:240-253）；URI scheme 依 RFC 3986 大小写不敏感，故 `MAGNET:?xt=...`（无 "://"）被归为 Local、`FILE:///...`（含 "://"）被归为 Network，Type 筛选会错分这类源。低危——应用自身生成小写 scheme，仅手改/第三方注入大写时误判。（src/app/ui/library.cpp:239-253）——属实：前缀匹配改为 `startsWithNoCase`（`src/app/ui/library.cpp:244-255`），新增 `FILE:///`/`MAGNET:` 大写用例（`tests/test_library.cpp`），Type 筛选不再错分；验证 `soar_library_tests` 28/28·229 断言绿
- [x] 审核发现：本轮 diff 未见问题（a87c6729..HEAD）——属实：区间仅 `1d114a8`（drain 守卫/原子设置/scheme 大小写/bframes 尾帧四修，已逐条复核落实）+ `7bab412`/`c6cb801` 两条 docs-only（仅 todo.md 挂账注记），无新问题；验证 `ctest --test-dir build --output-on-failure -j2` 15/15 全绿·274.70s（`SOAR_TEST_MEDIA`+`SOAR_TEST_BFRAMES_TAIL` 就位，backend_media 59.86s 含 drain 路径）
- [x] 审核发现：全局对代码块启用自动换行会在窄屏拆开现有文本树形图的长行，使续行失去树枝缩进且无法通过横向滚动查看原有布局，例如 docs/ui-design.md 的窗口结构图（docs/.vitepress/theme/custom.css:66）——属实：`config.mts` 的 markdown fence 钩子给含制表绘图字符（U+2500-U+257F）的代码块加 `no-wrap` 类（树形/状态机图：`docs/ui-design.md` 窗口结构图与 §3.1 状态机、zh 版同两处），`custom.css` 让 `no-wrap` 块回退 `white-space: pre`（`docs/.vitepress/theme/custom.css:72-78`），窄屏下长行不再断行、经块级 `overflow-x: auto` 横向滚动保列对齐；验证 `npm run docs:build` 14.60s 绿、构建产物仅这两处树形块带 `no-wrap`（其余代码块仍可换行）、`ctest --test-dir build --output-on-failure` 15/15 全绿·546.00s（`9c266cb`，`docs/.vitepress/config.mts:12-27`、`docs/.vitepress/theme/custom.css:72-78`、`docs/ui-design.md:67-88`）
- [x] 审核发现：config.mts:19 的 `html.replace('<div class="language-', '<div class="language- no-wrap ')` 在 `language-` 与语言名之间插入，把 VitePress fence 输出的 `class="language-{lang} vp-adaptive-theme"` 拆成 `class="language- no-wrap {lang} vp-adaptive-theme"`，破坏 `language-{lang}` 语义类完整性（拆为孤立 `language-` 与裸 `{lang}` 类，如 `text`/`cpp`），可能误命中其他 `.text`/`.cpp` 规则；且依赖硬编码前缀字符串，VitePress fence 输出格式一旦变更则 no-wrap 静默失效且无告警。（config.mts:19）——属实：`no-wrap` 改为 fence 显式标记，语言名保持 `language-text` 完整（`docs/ui-design.md:67,102`、`docs/zh/ui-design.md:67,102`）；包装输出时在完整 class 串末尾追加精确 `no-wrap`，若 VitePress fence 外层结构变更则构建失败而非静默失效（`docs/.vitepress/config.mts:15-30`）；验证 `npm run docs:build` 22.24s 绿，产物两处均为 `class="language-text vp-adaptive-theme no-wrap"`，且 `<span class="lang">text</span>` 保持不变
- [x] 审核发现：config.mts:16 的 `/[\u2500-\u257f]/` 仅匹配 Box Drawing 区段，树形/状态机图若改用其他绘图字符（U+2580-U+259F Block Elements、U+25A0-U+25FF Geometric Shapes，或纯 ASCII `+-|` 构图）则不标记 no-wrap，窄屏仍断行破坏列对齐；反之，任何含单个 box-drawing 字符的普通代码块（如示例文本中的 `└`/`│`）会被误标 no-wrap 强制横向滚动。（config.mts:16）——属实：取消按字符区段猜测，改为源文件显式 ```text no-wrap 标记，任何绘图字符/ASCII 构图的树形块均可选择 no-wrap，而含零散 `└`/`│` 的普通代码块不受影响（`docs/ui-design.md:67,102`、`docs/zh/ui-design.md:67,102`、`docs/.vitepress/config.mts:15-21`）；验证 `npm run docs:build` 绿，全站产物仅 4 处（两语言×两图）带 `no-wrap`，`ctest --output-on-failure -j2` 15/15 全绿·111.60s
- [x] 审核发现：custom.css:72-73 的 `div[class*='language-'][class*='no-wrap']` 用子串匹配 `'no-wrap'`/`'language-'`，未来若出现含这些子串的无关类（如 `no-wrapping`、`language-x`）会静默误命中；当前项目无此类类名，低危。（custom.css:72-73）——属实：CSS 选择器改为整词 `.no-wrap` class 匹配，不再对 `language-`/`no-wrap` 做子串匹配（`docs/.vitepress/theme/custom.css:72-76`）；验证 `npm run docs:build` 绿且产物 CSS 为 `.vp-doc div.no-wrap pre,.vp-doc div.no-wrap pre code{white-space:pre;word-break:normal}`，`ctest --output-on-failure -j2` 15/15 全绿·111.60s

- [x] 审核发现：四处树形/状态机图围栏由无语言标注的 ``` 改为 ```text no-wrap（docs/ui-design.md:67,102、docs/zh/ui-design.md:67,102），构建产物随之新增可见的 `text` 语言角标（`<span class="lang">text</span>`，此前为空、无角标）；`text` 对 no-wrap 机制并非必需——裸 ```no-wrap 即可触发且保持无角标，故角标为可规避的附带显示变化（低危/仅文档外观）——属实：围栏改为裸 ```no-wrap（`docs/ui-design.md:67,102`、`docs/zh/ui-design.md:67,102`），`config.mts` 钩子剥离 `no-wrap` 后 VitePress 以空语言渲染，`<span class="lang">` 为空、无 `text` 角标；验证 `npm run docs:build` 10.97s 绿，产物 4 处 no-wrap 块（两语言×两图）`span.lang` 均为空、无 `text` 角标，`ctest --output-on-failure -j2` 15/15 全绿·85.33s
- [x] 审核发现：custom.css:72-73 的 `.vp-doc div.no-wrap pre` 相对上一版丢掉 `[class*='language-']` 限定，仅凭 `no-wrap` 类命中，未来任何含 `no-wrap` 类的非代码 div（如布局辅助类）在 `.vp-doc` 内都会连带对其 `pre`/`pre code` 强制 `white-space: pre`；且该覆盖只靠与 custom.css:64-65（同为 specificity 0,2,2）的源码顺序取胜，重排或提升 64-65 特异性会静默回退为自动换行（低危，当前项目无此类类名）——属实：no-wrap 规则恢复 `[class*='language-']` 限定并提升特异性至 0,3,2（`.vp-doc div.no-wrap[class*='language-'] pre`/`pre code`，`docs/.vitepress/theme/custom.css:72-76`），不再凭源码顺序取胜、也不误命中非代码 div；验证 `npm run docs:build` 绿且产物 CSS 为 `.vp-doc div.no-wrap[class*=language-] pre,...{white-space:pre;word-break:normal}`，`ctest --output-on-failure -j2` 15/15 全绿·85.33s
## 源码分层分析 2026-11-07

### a) 抽象层清单

- L1 进程入口与窗口界面——解析 CLI、装配窗口配置、渲染 ImGui HUD/浮层并路由输入（`src/app/ui/player_window.cpp:727`）。
- L2 播放核心——以 `Player` 门面和 `IBackend` 契约隔离真实/测试解码实现（`include/soar/core/backend.h:108`）。
- L3 媒体服务——承载 HTTP 缓存、字幕发现/翻译、元数据检索和 ASS 渲染的辅助能力（`include/soar/core/http_cache.h:37`）。
- L4 P2P 桥——把 libtorrent 下载中的文件重新发布为可播放的 HTTP 字节范围源（`src/p2p/torrent_stream.h:53`）。
- L5 投屏控制点——发现 DLNA 渲染器、发送 AVTransport 命令、接收 GENA 事件并按需提供转码流（`src/cast/cast_session.h:39`）。

详细状态、通道、配置和生命周期见 `docs/architecture.md`；术语定义见 `docs/glossary.md`；拆分理由见 `docs/rationale.md`。中英文页面互相同步。

### b) 超大文件候选（>1000 行）

- [x] LAY-001 [P1] 拆分 `src/app/ui/player_window.cpp`（5273 行）——主题令牌/字体/状态文件加载/HUD 状态机/各类浮层/SDL 循环耦合在一个翻译单元（`src/app/ui/player_window.cpp:336`、`src/app/ui/player_window.cpp:487`、`src/app/ui/player_window.cpp:727`、`src/app/ui/player_window.cpp:5099`）。（2026-11-10 完成：HUD 类拆为 `src/app/ui/player_hud.h` + `player_hud_{actions,settings,render,overlays,pages}.cpp` 五个按职责的翻译单元，`player_window.cpp` 只留核心状态机/SDL 事件循环（~520 行）；新 TU 全文件 `SOAR_WITH_IMGUI` 门控，IMGUI=OFF 裸树验证过；全量 15 套 ctest + X11 CLI 636 断言绿。）
- [x] LAY-002 [P1] 拆分 `src/core/ffmpeg_backend.cpp`（4234 行）——AVIO 缓存、解码循环、字幕、HDR、SDL 音频和平台分支共享同一实现文件（`src/core/ffmpeg_backend.cpp:2416`、`src/core/ffmpeg_backend.cpp:3005`、`src/core/ffmpeg_backend.cpp:3467`）。（2026-11-10 完成：新增内部头 `src/core/ffmpeg_backend_internal.h`（libav/SDL include 集、文件级辅助函数、嵌套状态结构定义），实现拆为 `ffmpeg_backend{,_open,_decode,_video,_audio,_subtitle}.cpp` 六个翻译单元（生命周期+公共 API / 容器打开+解码器装配+avio 缓存 / 解码循环+帧队列 / 渲染+HDR+截图+取帧 / SDL 音频输出 / 轨道选择+字幕进出）；代码逐字搬移，逐行核对 decodeLoop 283/283 一致；全量 15 套 ctest 绿。）
- [x] LAY-003 [P1] 拆分 `src/core/subtitle_provider.cpp`（1174 行）——侧车扫描、HTTP 字幕客户端、翻译客户端和公共 HTTP/socket 代码同处一文件（`src/core/subtitle_provider.cpp:36`、`src/core/subtitle_provider.cpp:272`、`src/core/subtitle_provider.cpp:956`）。（2026-11-10 完成：新增内部头 `src/core/subtitle_provider_internal.h`（共享 socket/HTTP 原语：URL 解析、TCP 连接、收发、winsock 垫片；头内匿名命名空间=每 TU 内部链接，winsockInit 每 TU 一次 WSAStartup，引用计数无害已在头注释记档），翻译客户端拆为 `subtitle_provider_translate.cpp`（SubtitleTranslator + JSON/chat 辅助 + httpPost），`subtitle_provider.cpp` 留侧车扫描 + HTTP 下载客户端；公共 API（mediaHashHex/storeExternalSubtitle）定义保持在 soar 作用域；代码逐字搬移；subtitle 93/93·824 + core 33/33·246 绿。）

### c) 目录/分类不合理处

- `src/cast/transcode_server.cpp` 直接实现独立的 FFmpeg 解复用/解码/编码管线，但其媒体转换职责与 `src/core/ffmpeg_backend.cpp` 的播放管线不同，却都依赖同一 FFmpeg 基础能力；转码服务器放在 `src/cast/` 使 cast 目录同时承担控制面和媒体面（`src/cast/transcode_server.cpp:728`）。
- `src/app/ui/shortcuts.cpp` 声明不含 SDL 头文件的键码值，再用 `player_window.cpp` 的 `static_assert` 对照 SDL 值；跨层一致性由手工重复值保证，而不是独立键码抽象（`src/app/ui/shortcuts.cpp:18`）。
- `src/app/ui/player_window.cpp` 的路径解析、主题、设置持久化、字体选择与 UI 状态同属一个文件，而 `src/app/ui/ui_tokens.h` 已承担部分设计令牌职责；单一文件继续吸纳新偏好逻辑会扩大 LAY-001 的边界（`src/app/ui/player_window.cpp:487`、`src/app/ui/player_window.cpp:525`、`src/app/ui/ui_tokens.h:17`）。
- 无 SDL2 的 CLI-only 配置（CMake `SOAR_ENABLE_SDL2=OFF` 或装不上 SDL2 时提示 "building CLI-only app"）从未真正可编译：`player_window.cpp` 的 include 门从文件头一直开到尾，`namespace soar::app {` 在门内打开，无 SDL2 时整个命名空间体连同开闭括号一起消失，文件末尾 `}  // namespace soar::app` 成悬空括号（拆分前 HEAD~1 已实证同一错误：`WindowUiConfig` 不知名 + 悬空 `}`；2026-11-10 LAY-002 巡检中 no-SDL2 树复验）。要支持该配置需重构门结构（include 门提前收口、`SOAR_WITH_IMGUI`/`SOAR_WITH_FFMPEG` 门补 `SOAR_WITH_SDL2` 合取、runPlayerWindow 命名空间归属核对 `player_window.h` 声明）。CI 各 runner 均装有 SDL2，故从未暴露。

## 全界面按原型重做（2026-11-08 用户令：几十次强调按原型做）

- [ ] SOAR-UI-PROTO-002 [P0] 全界面视觉对齐原型：10-13 对齐批只覆盖了播放器 OSC（像素探针层），settings/library/playlist/translate-panel 等界面仍是 v1 占位实现（evidence/prototype-alignment-2026-11-01/after_m4_playback.png 实证：设置面板内容区是占位文本）。要求：以 docs/public/prototypes/*.html 四份原型为唯一视觉基准，逐界面（settings/library/playlist/translate-panel + 主播放窗全态）重做实现；验收口径改死：**每个界面渲染截图与原型 PNG 并排对比、逐项一致（布局/间距/圆角/字号/颜色/控件形态），vision 比对通过才算完**——测试断言绿不算完成；完成后并排对比图入 docs/review/，用户亲眼确认后勾账。
  - settings 已出对比图待用户确认（ec6c4c5，全量 15 套件绿，全页面布局+导航+5 个真实 Tab）：`docs/review/ui-proto-002/after/settings_{general,playback,subtitles,network,about}.png`、`docs/review/ui-proto-002/compare/settings_*.png`（compare/settings_subtitles.png 为原型 PNG 并排；其余 Tab 原型 PNG 只定义 Subtitles，compare 为通用版式）。**确认前保持未勾。**
  - library 已出对比图待用户确认（ad60122，全量 15 套件绿 + X11 CLI 77/77·636 断言，库页 224px 侧栏 LIBRARY/TYPE 分组+活动行高亮、head 条、140° 渐变海报（角值对齐原型 PNG）+源 pill+右对齐移除钮、walkthrough 探针按新几何重校）：`docs/review/ui-proto-002/after/library.png`、`docs/review/ui-proto-002/compare/library.png`（compare 为原型 PNG 并排）。**确认前保持未勾。**
  - 主播放窗全态已出对比图待用户确认（主窗本体本轮零代码改动——顶栏/状态徽标/OSC 布局由 fea5ecc 2026-11-01 与 10-13 批落地，本轮只出图与审计记账；60s lavfi 夹具、真 TTF 字体、playing=OSC 自动隐藏、controls=暂停钉栏两态）：`docs/review/ui-proto-002/after/main_{playing,controls}.png`、`docs/review/ui-proto-002/compare/main_{playing,controls}.png`（compare 为原型 PNG 并排）。差异注记随审计 §3.4 M1 行 + §7 决议后补记：原型 PNG 是移除前定稿帧（其顶栏不是规格，(a) 代决已去栏），而实现 fea5ecc 违 (a) 带顶栏、走查依赖之——留栏/拆栏待用户拍板。**确认前保持未勾。**
  - translate-panel：v2 契约下无此界面可重做——v2 redesign 未复刻翻译面板，翻译入口改为字幕菜单 `[translate]` 行（`docs/public/prototypes/README.md` 已载），既有字幕菜单走查（含失败端点臂）已覆盖该路径。
  - playlist 已出对比图待用户确认（5dd3385，全量 15 套件绿 + X11 CLI 77/77·636 断言 + 走查 22/22，playlist 全页面重做：74px 左 rail + head 带（loop 分段 All/One、shuffle、Add files、Clear 双击确认）+ 52px 行（hover 显出 next-play/移除双钮、now 条、now-playing 徽标）；排障中修了 InvisibleButton 命中区不落所绘坐标的 ImGui 陷阱（SetCursorScreenPos 钉位 + 行按钮裁掉动作区 + 行尾钉回行距），walkthrough 探针按新几何重校）：`docs/review/ui-proto-002/after/playlist.png`、`docs/review/ui-proto-002/compare/playlist.png`（compare 为原型 PNG 并排）。**确认前保持未勾。**
