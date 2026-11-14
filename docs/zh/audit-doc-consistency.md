
# 审计-文档一致性（2026-10-13）

> 注（2026-10-16 双语化）：本文的行号锚点指向改名之前的快照——中文稿现为 `*.zh.md`（顶部新增
> 语言切换器，行号整体后移 2 行），英文稿在裸名（`docs/mvp.md`、`docs/ui-design.md`、
> `docs/coverage-notes.md`，后者本轮只加了「暂无译版」说明页）。对照行号时按「切换器之前」计。

审计范围：README.md、README.zh.md、docs/index.md、docs/build.md、docs/mvp.md、
docs/ui-design.md、docs/licensing.md、docs/coverage-notes.md、docs/prototypes.md、
docs/alignment-audit-2026-10-13.md，外加公开门面（GitHub description / homepage /
topics 与 README 首段）。

口径：三类差异——**超前**（文档说有、代码无，最危险）/ **缺失**（代码有、文档漏，
只记实质项）/ **不符**（两边都有但措辞、参数、默认值、路径、行为不一致）。
每条主张附 `file:line` 源码证据；否定性结论先确认搜索路径有意义。修复一律
**以码为准**（删超前主张或标未实现、补缺失、改不符措辞），不为迁就文档改代码。

基线：`f1b245d`（本轮 transcode 修复提交）。工作区另有 OSC 批（鼠标移动算输入、
prev/next、A-B、Shot、剩余时间）在审计时未提交，凡涉及该批的行均已注明
「OSC 批本轮落地」，落库后对应行即闭合。

> **闭合注记（2026-10-15 复核）**：差异表 A01–A50 全部闭合——逐行对照当前
> `main`（`572e3ae`）复核，A01–A49 的修法均已落库（含 A31/A32/A37 的「OSC 批
> 本轮落地」注记、A23–A30 的行号/数字重锚），A50 核实一致无需修。本表自此是
> 历史快照，不再是待办台账；后续文档漂移按同口径另开审计，不在此表追加。

## 计数

| 类别 | 行数 |
|---|---|
| 超前（文档超前于码） | 3 |
| 缺失（码超前于文档） | 12 |
| 不符 | 35 |
| 合计 | 50 |

## P0 清单

1. **README.zh.md:33**「v1.0（硬解/HDR、投屏、媒体库）尚未开始」——媒体库
   （`src/app/ui/library.cpp:190,396`）、硬解首接线（`src/app/main.cpp:200`）、
   DLNA 投屏（`src/cast/` 全套）均已落地，与英文 README 直接矛盾，三大已交付
   功能被误述为未开始。
2. **docs/coverage-notes.md:24-25** §2 表「门禁 95.0% / 84.6%」——活体门禁
   （`.github/workflows/ci.yml:485`、本地六树脚本 `soar_sixtree.sh:102`）已
   重校为 **94.0 / 83.0**（§3.24:374 有重定记录），表未跟上。
3. **docs/coverage-notes.md:385** §5 结论「行 95.54% 与分支 85.15% 是当前
   代码库……真实上限」——转码批后水位为 94.44%（8886/9410）/ 83.50%
   （9500/11376）（§3.24:374-375），结论数字过期。

## 差异表

| ID | 页 | 主张 | 源码证据(file:line) | 类别 | 严重度 | 修法 |
|---|---|---|---|---|---|---|
| A01 | README.zh.md:33 | v1.0（硬解/HDR、投屏、媒体库）尚未开始 | 媒体库 `src/app/ui/library.cpp:190,396`；`--hwdec=` `src/app/main.cpp:200`；DLNA `src/cast/{ssdp,soap,transcode_server}.cpp` | 不符 | P0 | 改为「v1.0 两个切片已落地（媒体库、硬解首接线），DLNA 投屏已落地；HDR 与插件未开始」 |
| A02 | coverage-notes.md:24-25 | 门禁 95.0% / 84.6% | `.github/workflows/ci.yml:485`、`soar_sixtree.sh:102` 均为 `--fail-under-line 94.0 --fail-under-branch 83.0`；重定记录 `docs/coverage-notes.md:374` | 不符 | P0 | §2 表门禁列改 94.0/83.0，实测列刷新为 §3.24 水位 94.44/83.50 |
| A03 | coverage-notes.md:385 | 行 95.54% 与分支 85.15% 是当前真实上限 | `docs/coverage-notes.md:374-375`：转码批后水位 94.44%（8886/9410）/ 83.50%（9500/11376） | 不符 | P0 | §5 结论数字刷新到 §3.24 水位，95.54/85.15 标注为转码批前读数 |
| A04 | README.zh.md | 整条「媒体库」功能缺失（watch folders/历史/续播/B 浮层/NFO） | `src/app/ui/library.cpp:190,396`；`src/app/ui/shortcuts.cpp:207`（kKeyB） | 缺失 | P1 | 功能特性补「媒体库」条目（照英文 README 同条译） |
| A05 | README.zh.md | 视频渲染条缺 `--hwdec=` 硬件解码与软解回退句 | `src/app/main.cpp:200-206`；`src/core/ffmpeg_backend.cpp:77-101,2855` | 缺失 | P1 | 该条末补「--hwdec= 请求硬解、失败回退软解、hwaccel 帧回传 YUV420P」 |
| A06 | README.zh.md | 投屏（DLNA 控制点、`t` 浮层、`--cast`）整条缺失 | `src/cast/{ssdp,soap,eventing,device_xml,transcode_server}.cpp`；`src/app/main.cpp:224-231`；`src/app/ui/shortcuts.cpp:210` | 缺失 | P1 | 当前状态/功能特性补投屏条目 |
| A07 | README.zh.md | 快捷键列表缺 `B`（媒体库浮层） | `src/app/ui/shortcuts.cpp:207` | 缺失 | P2 | 列表 `O` 后补 `B` |
| A08 | README.zh.md | 缺「全部可经 soar/shortcuts.txt 重绑、--shortcuts= 移动、--no-shortcuts 禁用」句 | `src/app/ui/shortcuts.cpp:445-454`；`docs/ui-design.md:108` | 缺失 | P1 | 快捷键条末补该句（照英文 README 同条译） |
| A09 | README.zh.md | 使用示例缺 `--hwdec=auto` / `--no-resume` / `--shortcuts=` / `--no-shortcuts` | `src/app/main.cpp:60-64,200` | 缺失 | P2 | 使用代码块补四条示例 |
| A10 | README.zh.md | 测试表缺 `soar_dir_scan_tests`/`soar_library_tests`/`soar_shortcut_tests` 三行 | `tests/CMakeLists.txt:146,159,173` | 缺失 | P1 | 测试表补三行 |
| A11 | README.md + README.zh.md | 测试表缺 `soar_cast_tests` 行 | `tests/CMakeLists.txt:119` | 缺失 | P2 | 两表各补一行 |
| A12 | README.zh.md:155 | 路线图 v1.0 把「投屏（AirPlay/Chromecast/DLNA）」并列 | 全树无 AirPlay/Chromecast 代码（选型见 `todo.md` 投屏协议选型节，DLNA 为唯一入选） | 超前 | P2 | 改为「投屏（DLNA/UPnP）」，AirPlay/Chromecast 移出或注明不在范围 |
| A13 | README.zh.md:33 路线图 | 「媒体库与刮削」列为 v1.0 未开始 | 媒体库与离线 NFO 已落地 `src/app/ui/library.cpp:190,396` | 不符 | P1 | 改为「媒体库与离线 NFO 已落地；网络刮削与多设备同步待定」 |
| A14 | README.md | "Demux and decode run on separate threads" | 唯一解码线程 `src/core/ffmpeg_backend.cpp:757`，`av_read_frame` 与解码同在该线程（`:2792`） | 不符 | P2 | 改为「解封装与解码同在独立解码线程，播放/UI 线程不被 IO 阻塞」 |
| A15 | README.md | "four event categories — state changes, progress updates, media info changes, errors" | `include/soar/core/backend.h:63-79` 实为 6 类（另有 BufferingStarted/Ended、DownloadProgress） | 不符 | P2 | 改为「四类核心事件（另有缓冲与下载进度事件）」 |
| A16 | README.md | "Backends are pluggable: FFmpeg, system frameworks, and fake backends" | `src/core/` 仅 `ffmpeg_backend.cpp` + `null_backend.cpp`；「system frameworks」全树仅此一处 | 超前 | P2 | 删「system frameworks」或改「（系统框架后端预留）」 |
| A17 | README.md | mermaid 图 "Core library soar_core (pure abstraction, Apache-2.0 core)" | `CMakeLists.txt:60-66`：探测到 FFmpeg 时 `ffmpeg_backend.cpp` 编入 `soar_core` 并 PUBLIC 链接 `PkgConfig::FFMPEG` | 不符 | P2 | 改为「核心库 soar_core（接口纯抽象；FFmpeg 后端按探测编入）」 |
| A18 | README.md | "the CLI `--cast` flag" | `src/app/main.cpp:224-227` 只接受 `--cast=<location\|auto>`；裸 `--cast` 落入路径解析 | 不符 | P1 | 改为 `--cast=<location\|auto>` |
| A19 | README.md | 快捷键列表缺 `t`（投屏浮层） | `src/app/ui/shortcuts.cpp:210` | 缺失 | P2 | 列表补 `t` |
| A20 | README.md | "torrent bridge URLs are skipped (their local port changes every session)" | 被跳过的是原始 magnet/.torrent URI（`src/app/ui/player_window.cpp:478-481`）；桥接 URL 反而被 `recordOpen` 记录（`:810`） | 不符 | P1 | 改为「原始 magnet/.torrent URI 不入历史；桥接 URL 会记录（端口每会话变化）」 |
| A21 | README.md | "docs/mvp.md §3, docs/todo.md" | 仓库根为 `todo.md`，无 `docs/todo.md` | 不符 | P2 | 改为 `todo.md` |
| A22 | coverage-notes.md:24-31 等 | 多处「本地 cov4 完整跑」 | 六树为 system/cov3/tsan/tsan-o0/asan/noimgui（`soar_sixtree.sh:10`），无 build-cov4 | 不符 | P1 | cov4→cov3 |
| A23 | coverage-notes.md §3.6 | player_window.cpp 行号锚点（DROPFILE 359、toast 427、字体回退 206 等） | 现文件 3131 行：DROPFILE `player_window.cpp:757`、toast `:1189-1251`、字体回退 `:398/:428` 等 | 不符 | P1 | §3.6 行号按现文件重锚 |
| A24 | coverage-notes.md §3.7 | http_cache.cpp 行号表（407/410、606-608 等） | 现 705 行；`kRecvTimeoutSec=60` 在 `:38` ✓ | 不符 | P1 | §3.7 行号按 705 行文件重锚 |
| A25 | coverage-notes.md §3.9 | "test_backend_media.cpp:1051" | 实际 `tests/test_backend_media.cpp:1741` | 不符 | P2 | 改行号 |
| A26 | coverage-notes.md §3.13/§3.22 | ass_dialogue.cpp 136/137 | 现 281 行 | 不符 | P2 | 标注为批时读数或刷新 |
| A27 | coverage-notes.md §3.15/§3.17 | 分文件分母 player_window.cpp 1090/1147、ffmpeg_backend.cpp 1712/1866 | 现 3131 / 3763 行 | 不符 | P1 | 标注为批时读数或刷新 |
| A28 | coverage-notes.md §3.19 | "soar_p2p_tests（tests/test_p2p.cpp，20 用例）" | `tests/test_p2p.cpp` 实测 21 个 TEST_CASE | 不符 | P1 | 20→21 |
| A29 | coverage-notes.md §3.23 | "cast 套件 57 例" | `f1b245d` 后 58 例 | 不符 | P2 | 57→58 |
| A30 | coverage-notes.md §1 | "hermes PATH 上默认解析到 pip 的 gcovr 8.6"；"CI 的 apt 是 7.0（ubuntu noble）" | 本机 `command -v gcovr`=/usr/bin/gcovr 7.2；hermes 环境离线无法核实；`ci.yml` 用未钉版 ubuntu-latest 矩阵 | 不符 | P2 | 改为「跑门禁前先 `command -v gcovr` 核对」；CI 镜像说法按当前 runner 核实 |
| A31 | alignment-audit-2026-10-13.md §1 差异3 | 「OSC 无 A-B/CC/截图/prev/next 按钮；无剩余时间、无画质 pill」 | HEAD 属实（无对应按钮）；OSC 批（工作区未提交）已加全部这些按钮+剩余时间 | 不符 | P1 | 表头注记：OSC 批本轮落地后该行闭合 |
| A32 | alignment-audit-2026-10-13.md §1 | 「鼠标移动不算输入（SDL_MOUSEMOTION 无处理）」 | HEAD 属实；OSC 批已加 `case SDL_MOUSEMOTION` | 不符 | P1 | 同上注记 |
| A33 | alignment-audit-2026-10-13.md §5 | 「无 Translate overlay、无快捷键」 | `shortcuts.h:116-141` 确无 Translate ✓；但 env 门控的 [translate] 菜单行 + `translateTextAndLoad` 已存在（`player_window.cpp:490-513,1189-1251`） | 缺失 | P2 | 补记该部分实现（面板未做、菜单行与翻译管线已有） |
| A34 | alignment-audit-2026-10-13.md §1 | 「toast（Paused）」 | 无 Toast；是 "Paused" chip（`player_window.cpp:2122-2130` drawChips） | 不符 | P2 | 措辞改 chip |
| A35 | alignment-audit-2026-10-13.md §2 | 「当前行高亮（≻ 标记 + 高亮行）」 | 标记是 `"> "`（`player_window.cpp:2554`），非 "≻" | 不符 | P2 | 措辞改 `> ` |
| A36 | alignment-audit-2026-10-13.md §4 | 「字幕轨道选择在『Subtitle Settings』浮层（OSC Sub 按钮）」 | 选轨是 OSC Sub 组合框（`drawTrackCombo` `:1960`，调用点 `:1925`）；"Subtitle Settings"（`:2725`）是字号/偏移/可见性浮层 | 不符 | P2 | 区分两者 |
| A37 | ui-design.md:100 | §3.1「隐藏 ──(任意按键/鼠标移动/滚轮/单击)──▶ 可见」含鼠标移动 | HEAD 无 SDL_MOUSEMOTION 处理；OSC 批已加 | 超前 | P1 | 表头注记：OSC 批本轮落地后「鼠标移动」成立 |
| A38 | ui-design.md:110-133 | §3.2 表「上表即出厂默认」但无 `t` 行 | `src/app/ui/shortcuts.cpp` addDefault(Cast, kKeyT)；浮层 `player_window.cpp:1477` | 缺失 | P1 | 表内补一行 `t` 投屏浮层 |
| A39 | ui-design.md:72-83 | §2 浮层树无 cast 浮层条目 | `src/app/ui/player_window.cpp:1477` drawCastOverlay + t 键绑定 | 缺失 | P1 | §2 树补「投屏浮层（t 键）」 |
| A40 | ui-design.md:182 | 「OSC 高 ~52px」 | `src/app/ui/player_window.cpp:1835` bar_h=80.0f | 不符 | P2 | 改 ~80px |
| A41 | ui-design.md:182 | 「浮层宽上限 720px」 | 信息浮层 max 480（`player_window.cpp:2263`、`:2724`）；OSC bar max 920（`:1834`）；全树无 720 | 不符 | P2 | 改 480px |
| A42 | ui-design.md:179 | 「面板 rgba(16,18,24,0.92)」 | `player_window.cpp:335` WindowBg alpha 0.94 | 不符 | P2 | 改 0.94 |
| A43 | ui-design.md:69 | 「缓冲指示（左上角 "缓冲中…"）」 | `player_window.cpp:2089` chip 文案 "Buffering..." | 不符 | P2 | 改 "Buffering..." |
| A44 | ui-design.md:69 | 「下载指示（左上角 "下载中 N%"）」 | `player_window.cpp:2097` "Downloading N%" | 不符 | P2 | 改 "Downloading N%" |
| A45 | ui-design.md:79,130 | 「媒体库浮层（B 键，v0.3）」 | 已落地：`shortcuts.cpp` Library→B；`player_window.cpp:2546` drawLibraryOverlay | 不符 | P2 | 删 "v0.3" 或注「已落地」 |
| A46 | mvp.md:37 | 状态变化事件：opening/playing/paused/stopped/ended/error | `include/soar/core/backend.h:54-60` PlaybackState 无 Opening（打开失败走 Error 事件） | 不符 | P2 | 删 "opening/" |
| A47 | mvp.md:25 | 倍速（0.5x/1.0x/1.5x/2.0x） | `src/app/ui/player_window.cpp:1945` kRates={0.5,0.75,1.0,1.25,1.5,2.0} | 不符 | P2 | 改「0.5–2.0 六档」 |
| A48 | mvp.md:83 | soar_p2p_tests 20 用例 | `tests/test_p2p.cpp` 21 个 TEST_CASE | 不符 | P2 | 改 21 |
| A49 | ui-design.md:229 | 「后端自身要求 2.0.17（SDL_RenderGeometry）」 | `CMakeLists.txt:235-236` 注释同写 2.0.17；SDL_RenderGeometry 实为 SDL 2.0.18 引入 | 不符 | P2 | doc 与 CMake 注释两处同改 2.0.18 |
| A50 | 公开门面 | description「built with FFmpeg, SDL2 and libass (C++17)」、homepage、13 topics | 全部属实：`vcpkg.json`、`docs/.vitepress/config.mts:12`（base '/soar/'）、`.github/workflows/deploy-docs.yml`、`src/core/http_cache.cpp`、`CMakeLists.txt:13`（SOAR_ENABLE_IMGUI 可选） | — | — | 核实一致，无需修 |

## 已核实一致（抽查，无需修）

- README 31 条快捷键出厂默认、`--cache-dir=`/`--torrent-*=`/`--no-resume`/`--shortcuts=`/`--no-shortcuts`/`--gui`/`--headless`/`--backend=`、环境变量（`SOAR_SUBTITLE_ENDPOINT/_API_KEY`、`SOAR_TRANSLATE_ENDPOINT/_API_KEY`、`SOAR_INSTALL_DIR`、`SOAR_TEST_X11`）。
- glibc≥2.38 / GLIBCXX_3.4.32、包名（zip/deb/rpm/AppImage/`soar-setup-windows-x64.exe`/`SHA256SUMS.txt`）、安装目录、`xattr -cr`、`screenshot.png`、MRU×15、OSC 2.5s + 四钉住、Loop{Off,All,One}、Shuffle、A-B 三步、字幕解码门、`resumePositionMs` 5s/5s、libtorrent v2.0.15、FFmpeg 四库、CMake 3.20、SDL2≥2.0.18、13 测试套件、gcovr 目录形 `--filter`、Apache-2.0、HLS/DASH/RTSP 冒烟、libass MIT、`soarw.exe` WIN32 目标、SSDP/SOAP/GENA/转码 H.264+AAC MPEG-TS、插件体系未开始。
- 对齐审计的 OSC 自动隐藏（`kHideDelay=2500` `ui_state.h:46`、四钉住 `ui_state.cpp:91-101`、0.16s 淡出 `player_window.cpp:835`）、设置页未实现、translate 面板未实现、7 张证据 PNG 全在、todo.md 28 勾 + 7 未勾。

## 增量（2026-11-02，批 G5）

| ID | 页 | 主张 | 源码证据 | 类别 | 严重度 | 修法 |
|---|---|---|---|---|---|---|
| G1 | ui-design.md §7 + zh 双语 | 「仍不做的：…seek 悬停缩略图…设置面板」 | 两者 2026-11-02 已落地（`0d07d89`）：设置页（G 键，`SettingsTab` `player_window.cpp:198`、`drawSettingsOverlay` `:2299`）、seek 缩略图（ThumbnailService `:747` + `drawThumbPreview` `:2968`）、媒体库 Type 筛选（`matchesSource` `:4346`/`:4369`） | 码超前于文档 | P1 | §7「已落地」清单补三项（中英），「仍不做」收窄为文件对话框/迷你模式/多主题 |
| G2 | ui-design.md §2 + zh 双语 | 「v0.1 无设置页…v0.2 再做成面板」 | 同 G1 证据，面板已交付 | 码超前于文档 | P1 | 中英两侧改为「设置面板已于 2026-11-02 落地（G 键）」 |
| G3 | prototype-alignment-audit.md M4/M7 + zh 双语 | 「无——有意延后（v0.2）」 | 同 G1 证据 | 码超前于文档 | P2 | 行内注记「2026-11-02 已落地（`0d07d89`）」并附代码锚点（历史判定保留） |

本增量三桶计数：码超前于文档 **3**（G1–G3）· 文档超前于码 **0** · 描述不符 **0**。P0：无。全部仅改文档；该发现由 2026-11-02 审核批先登记进 `todo.md`，本轮收口（勾账留给台账归属会话）。

文案去味（同轮）：README.md 轨道切换句「seamless while playing or paused」→「works while playing or paused, without interrupting playback」；`docs/zh/coverage-notes.md` ⭐ 装饰标记改粗体导语、「（必读…）」标题软化。历史快照节（alignment-audit-2026-10-13.md、prototypes.md 审计叙述）按收口惯例保留为带日期记录，不回改。


## 增量 G2（2026-11-14，舰队文档审计）

范围：六个高漂移面——功能清单、CLI 参数、快捷键表、平台/产物矩阵、套件表、徽章
与 About 行——对照 `9552d62` 的代码逐条核对，两个 README 版本与文档站一并覆盖。
严重度按规范：文档超前于码 = P0，产物/清单漂移 = P1，措辞/链接 = P2。

| ID | 页 | 主张 | 源码证据 | 类别 | 严重度 | 修法 |
|---|---|---|---|---|---|---|
| G2-1 | README.md 功能节、README.zh.md:38 | 英文功能清单缺投屏条（zh 版本有）；`--cast-iface=` 与 `--open-dialog` 命令行接受却无任何文档 | 两者同批落地于 `599ec4e`；`src/app/main.cpp:257` 与 `:264` 解析，`:66`、`:84` 出现在 `--help`，`player_window.cpp:821` 消费 | 码超前于文档 | P1 | 英文功能清单补 **投屏** 条（对齐 zh）并载 `--cast-iface=`；`--open-dialog` 写入两版键鼠交互条 |
| G2-2 | README.md、README.zh.md 功能节 | 设置浮层（`G`）与明暗外观行（`y`）两版功能清单皆无 | `OpenSettings`→`kKeyG` `src/app/ui/shortcuts.cpp:202`、`ToggleTheme`→`kKeyY` `:200`、`shortcuts.h:142`；五页签 `player_hud_settings.cpp:467-487`，外观卡 `:588`、Dark/Light 分段 `:260`；`loadThemePref` `player_window.cpp:1077` | 码超前于文档 | P1 | 两版各补 **设置浮层** 条，列五页签与可持久化的外观选择 |
| G2-3 | README.md:41、README.zh.md:42 快捷键行 | `G`、`y`、`/`、`Ctrl+O` 四键未列 | `shortcuts.cpp:198`（`SearchLibrary`→`kKeySlash`）、`:200`（`ToggleTheme`→`kKeyY`）、`:202`（`OpenSettings`→`kKeyG`）、`:188`（`OpenFolder`→`Ctrl+O`）；`/` 处理见 `player_window.cpp:656` | 码超前于文档 | P1 | 两版快捷键行各补四键 |
| G2-4 | README.md、README.zh.md 套件表 | 英文表 13 行、zh 表 13 行，而构建实际注册 15 个套件 | `tests/CMakeLists.txt:117`（`soar_p2p_tests`）、`:82`（`soar_metadata_tests`）、`:206`（`soar_uri_tests`）；英文表缺 `soar_p2p_tests`，zh 表缺 `soar_metadata_tests` | 码超前于文档 | P1 | 两表补齐至 15 个注册套件；新行按各自 CMake 头注描述（进程内自建的 P2P、fork 夹具 HTTP 服务的元数据、`file://` 解码的 uri） |
| G2-5 | README.md、README.zh.md | iOS 切片（`ios/SoarKit`、`ios/Bridge/soar_bridge.h`、`ios.yml`）两版 README 全无 | `ios/SoarKit/{Package.swift,Sources,Tests}`、`.github/workflows/ios.yml:22-33` 的 `xcodebuild` 构建与测试（`ios/**` 推送触发）、`ios/README.md` 状态表（SoarKit CI 已验证；桥未接线；核心交叉编译与 App 壳未开始） | 码超前于文档 | P1 | 两版路线图各补 **iOS 切片** 条，写明已 CI 验证的范围与两项未开始 |
| G2-6 | README.zh.md 链接 | 四个 zh 链接目标 `docs/{build,mvp,licensing,coverage-notes}.zh.md` 从未存在（8 处：4 链接 + 4 提及） | `f0f2c33` 把 zh 页移入 `docs/zh/`；`ls docs/zh/` 四项均可解析 | 文档超前于码（死链） | P1 | 8 处全部改指 `docs/zh/<name>.md`，逐个验证可解析 |
| G2-7 | docs/prototypes.md:63、docs/zh/prototypes.md:67 | 「未实现 2 屏（设置面板、翻译浮层）」 | 设置面板 2026-11-02 已交付（`0d07d89`）；翻译面板经 v2 契约取消，入口移入字幕菜单的 `[translate]` 行（`todo.md:180`） | 描述不符 | P1 | 就地补注现状；带日期的原判定按 G3 惯例保留为历史记录 |

文案与指针修（同轮）：README.md:40 失效的 `docs/ui-design.md §3.2` 引用删除（P2，该文件已于 2026-11-11 的 `58b9b57` 删除）；README.zh.md 同处引用一并删除；`docs/prototypes.md` 的 `ui-design §2` 字样作为历史归属保留（它记录的是被取代的旧稿当时怎么写的），不作指针。

本增量三桶计数：文档超前于码 **0** · 码超前于文档 **6**（G2-1…G2-6）· 描述不符 **1**（G2-7）。P0：无——文档声称的每个功能都核实存在（`--cast-transcode` 见 `src/app/main.cpp:195,255`；插件体系确实未开始；夜间构建产物清单与 `nightly.yml:522-540` 及 README:112 相符；preset 与 README:107/111 的平台边界与 workflow 矩阵一致）。全部仅改文档，未为迁就文档改动任何代码。

### 浏览器语言自动识别（同轮）

形态：成对文件（`README.md`/`README.zh.md`）**且** vitepress locale 路由——`docs/.vitepress/config.mts:37-40` 声明 `locales.root` 为 `lang: 'en'`（英文在站点根），`:86` 为 `zh/` 声明 `lang: 'zh-CN'`，`base: '/soar/'`；英文 12 页对中文 12 页，完全对称。

自动识别：**已具备**。`docs/index.md`（根落地页）带一段 `<script setup lang="ts">`：先查 `localStorage`，再查 `navigator.languages`/`navigator.language`，任一 locale 以 `zh` 开头即把 URL 换成 `/soar/zh/`；zh 落地页带同样脚本指回根。点击选择会被记录，后续自动检查让位。两个落地页都有。

线上 URL 契约探针（2026-11-14 实测）：

| URL | HTTP | `<html lang>` | 首屏 CJK |
|---|---|---|---|
| `https://cuihairu.github.io/soar/` | 200 | `en` | 223 |
| `https://cuihairu.github.io/soar/zh/` | 200 | `zh-CN` | 447 |

根页出 `en` 与本地配置一致，是正确的；英文落地页上那 223 个 CJK 字符来自语言切换器标签与原型预览的导航名，不是部署产物滞后。舰队层面「本仓根页出中文」的说法未复现，无需重新部署，也未改动任何配置。无需 todo：自动切换增强项本仓已存在。
