# UI 设计（v0.1 桌面版）

> 目标：给现有"核心播放抽象 + FFmpeg 后端"补上第一版可用桌面 UI——能打开、播放、暂停、seek、切轨、显示媒体信息。本文先沉淀对成熟播放器的调研结论，再给出信息架构、控件交互、视觉风格与技术选型，作为实现依据；v0.2+ 的扩展（播放列表页、缩略图预览、迷你模式）只画边界不展开。

## 1. 调研结论

调研对象：IINA（macOS 现代播放器标杆）、mpv.net（Windows 桌面现代 GUI）、vidstack / plyr（现代 Web 播放器）、awesome-mpv 收录的 OSC 生态、Qt6+QML 播放器实现参考。以下均为从仓库 README/手册/源码默认值中提取的**可核实事实**，不是臆测。

复核入口（默认值都在源码里，改版即失效，值得复查）：IINA 的 `iina/Preference.swift`（`defaultValue` 字典与各 `defaultValue` 静态成员，如 `controlBarAutoHideTimeout: Float(2.5)`、`thumbnailWidth: 120`、`maxThumbnailPreviewCacheSize: 500`）与 `develop` 分支；mpv.net 的 `src/MpvNet/App.cs`（`RecentCount = 15`）与 `src/MpvNet.Windows/WinForms/MainForm.cs`；vidstack/plyr 的默认 layout 源码与主题令牌（`--plyr-*` / `--media-*`）。

### 1.1 IINA（github.com/iina/iina）

- OSC（屏上控制器）默认**悬浮居中**于底部上方（`oscPosition = floating`，`controlBarStickToCenter = true`），**2.5 秒无输入自动隐藏**（`controlBarAutoHideTimeout = 2.5`，默认开启）。
- **单击视频区 = 显示/隐藏 OSC**（`singleClickAction` 默认 `hideOSC`），双击 = 全屏——把"单击暂停"让位给"双击全屏"，避免双击手势的第二击误触发暂停。
- 滚轮语义分离：**垂直滚轮 = 音量，水平滚轮 = seek**（`verticalScrollAction = volume`、`horizontalScrollAction = seek` 默认值）。
- 进度条悬停**缩略图预览**默认开启（`enableThumbnailPreview = true`，缩略图宽 120px，缓存上限 500 张）。
- 音乐模式：检测到纯音频**自动切换**迷你窗口（`autoSwitchToMusicMode = true`，显示专辑封面）。
- 恢复播放与历史默认全开：`resumeLastPosition`、`recordPlaybackHistory`、`recordRecentFiles` 均 true；播放结束**保持窗口打开**（`keepOpenOnFileEnd = true`）。
- 播放列表是主窗口内的侧栏（第三方插件做"分离式播放列表窗口"恰恰反证内置侧栏形态）。

### 1.2 mpv.net（github.com/mpvnet-player/mpv.net）

- 不做常驻工具栏，用 **OSC + 扁平化设计**；主菜单走**右键上下文菜单**（可在 input.conf 用 `#menu:` 自定义层级）。
- **暗色是默认**：`--dark-mode=always`（默认 always），主题（`--dark-theme`）独立可换。
- 最近文件：记住 **15 条**（`--recent-count` 默认 15）；播放列表为空时 `play-pause` 命令自动加载最近一条。
- OSC 死区：拖拽窗口/呼出菜单/隐藏鼠标只在窗口**中心区域**生效（四周留 10%/底部 22% 给 OSC），避免与控件抢事件。
- 配置用**可搜索的 GUI 配置编辑器**替代手改 conf 文件。

### 1.3 mpv 生态 OSC（awesome-mpv 收录）

- 事实上的"现代 OSC"标准形态由 **uosc / ModernX / ModernZ / osc-modern** 系列确立：底部悬浮条 + 大 seek 条 + 左传输右设置，条上按钮可配置。
- seek 条增强的通用做法：**悬停时间提示**（几乎所有 OSC）、**章节刻度**（chapters 脚本）、**悬停缩略图**（tethys、thumbnail_script、thumbfast 高性能缩略图服务）。
- 极简派用**常驻细进度条**（progressbar、mfpbar——小、不挡画面、可拖动），说明"信息密度可降级"是有效形态。
- uosc 生态还有 history 菜单、暂停指示器（pause-indicator）等配套，验证了"浮层 + 菜单"组合的扩展模型。

### 1.4 Web 播放器（vidstack / plyr）

- 控制条形态（vidstack 默认 video layout 源码）：**双行**——第一行是占满整宽的时间滑条，第二行是按钮行（播放 → 音量 → 时间 → 字幕开关 → 设置菜单 → 全屏）；滑条拿到全宽抓取区，按钮有独立密度。单行三段式（左传输/中 seek/右设置）则是 plyr 的形态。
- seek 条交互细节：**悬停显示目标时间、拖动实时预览、松手才提交**（vidstack scrub-preview）；键盘步进为 `keyStep` + Shift×5 倍乘（vidstack slider 默认）；vidstack 默认 seek 步长 10s，plyr `seekTime` 也是 10s。
- 设置菜单组织：vidstack 单一齿轮菜单，内含**倍速 → 画质 → 无障碍 → 音轨 → 字幕**五个子菜单（单选组）；plyr 设置面板为"字幕/画质/倍速"分组 + 返回上一级。当前值打勾标注，均不超过一级嵌套。
- 快捷键高度趋同（YouTube 系）：`K/Space` 播放暂停、`J/L` ∓seekStep（vidstack）、`←/→` 短 seek、`↑/↓` 音量、`M` 静音、`F` 全屏、`C` 字幕、`0–9` 百分比跳转（plyr 语义；mpv 把 1–8 留给画质调整，对普通用户反直觉）、`</>`倍速增减（vidstack）。
- 无障碍被当作一等公民：slider 暴露 `aria-valuemin/max/now/text`；开关按钮带 `aria-pressed` 与成对的按下/未按下图标+标签（plyr "Play"/"Pause"）；菜单按钮带 `aria-label`；焦点可见态（focus-visible）与专用焦点环 token（vidstack `--media-focus-ring`）；快捷键自动镜像到控件 `aria-keyshortcuts`。
- 主题全部走设计令牌：vidstack 每个令牌有 `--media-*` 语义名 + `--video-*` 布局覆盖两层；plyr 是平铺 `--plyr-*` 属性（含 `--plyr-color-main`、`--plyr-focus-visible-color`、控件尺寸/间距/圆角等全套）。控制按钮的悬停提示默认只给读屏器（视觉提示仅 seek 时间）——保持界面安静。

### 1.5 Qt6+QML 桌面实现参考

- Qt-Music-Player 这类项目验证的通用模式：**C++ 引擎层（播放/队列/元数据）通过 QObject 属性与信号暴露给 QML 视图层**，页面（播放页/列表页/设置页）是纯 QML，动画/主题用 QML 原生能力。对本项目的启示：**UI 层必须只依赖 Player 门面**（我们已有等价的"核心/UI"分层），换 UI 框架不动核心。

### 1.6 跨框架可迁移的设计决策（本文的实现依据）

1. 控制栏：底部悬浮、内容居中、自动隐藏 ~2.5s；**指针悬停栏上/拖动 seek/浮层打开/暂停时钉住**（IINA + vidstack 共识）。
2. 单击视频区 = 切换控制栏显隐，双击 = 全屏（IINA 语义，规避双击误触暂停）。
3. 垂直滚轮 = 音量，水平滚轮/Shift+滚轮 = seek（IINA 默认）。
4. 控制条双行：整宽 seek 条在上、按钮行在下（vidstack 默认布局与 IINA 悬浮控制器共同收敛的形态）；二级选择收进弹出菜单（vidstack/plyr/uosc 共识）。
5. seek 条：悬停时间提示 + 拖动实时预览 + **松手提交**；缩略图预览是 v0.2 增强（IINA 120px 宽、500 张缓存的规格可作参照）。
6. 快捷键采纳 YouTube/mpv 并集：`Space/K`、`←/→ ±5s`、`Shift+←/→ ±1s`、`PgUp/PgDn ±60s`、`↑/↓ 音量`、`M`、`F`、`0–9 百分比`、`A 切音轨`、`C 切字幕`、`I 信息`、`R 最近`、`H 帮助`。
7. 暗色主题为默认且不可切换（mpv.net `dark-mode=always` 默认值的思路：视频播放器的背景天然是黑的，浅色 UI 从来不是正确默认）。
8. 最近打开默认记录（IINA/mpv.net 均默认开启，15 条量级），作为 v0.2 播放列表的种子数据。

## 2. 信息架构（v0.1）

v0.1 是**单窗口应用**：一个播放页承载一切，二级功能全部是浮层（overlay），不引入多页面路由。

```
soar 主窗口
├── 视频区（信箱式留黑，等比缩放）
│   ├── 暂停指示（大图标，1s 淡出）          ← v0.1 简化为状态文案
│   ├── 缓冲指示（左上角 "缓冲中…"）          ← BufferingStarted/Ended 事件驱动
│   ├── OSD Toast（顶部居中，800ms 淡出）     ← 音量/倍速/切轨操作反馈
│   └── 无媒体海报区（文件名 + 状态文案）      ← 纯音频/null 后端时
├── OSC 控制栏（底部悬浮，自动隐藏，双行）
│   ├── 行 1：seek 条（整宽，悬停时间提示、拖动预览、松手提交）
│   ├── 行 2：⏯ 播放/暂停、⏹ 停止、当前/总时长 | 🔇/🔊+音量条、
│   │         倍速、音轨、字幕 | ⓘ 信息、⛶ 全屏
│   └── 弹出菜单：倍速 / 音轨 / 字幕（各一组，当前项打勾）
├── 信息浮层（I 键）：文件、状态、时长、可否 seek、后端、轨道清单（类型/编号/编码/语言/标题/选中）
├── 最近打开浮层（R 键）：MRU 列表，点击重开；无历史时提示
├── 快捷键帮助浮层（H 键）：静态键位表
└── 退出路径：Esc（全屏时先退全屏）/ 窗口关闭按钮
```

**打开媒体的入口**（v0.1）：命令行 URI、拖放文件到窗口（SDL drop 事件）、最近打开浮层。文件对话框留给 v0.2（需要 SDL2 封装或平台代码，先不引入）。

**轨道与字幕选择**：OSC 弹出菜单（当前项打勾）；快捷键 `A`/`C` 循环切换（mpv 语义）；`C` 循环含"关闭"。字幕轨在 v0.1 仍是元数据选择（字幕渲染未实现，见 docs/mvp.md §2），菜单照常列出并标注。

**设置**：v0.1 无设置页。主题常量、自动隐藏时长（2500ms）、Toast 时长（800ms）、最近上限（15）集中定义，v0.2 再做成面板。

**状态模型**：UI 可见状态 = 播放器状态（`PlaybackState`）× 输入态（控制栏显隐、Toast、缓冲标志）× 浮层态（info/recent/help，互斥单开）。后端错误显示为 Toast + 信息浮层内的错误行，不弹模态框（播放器错误极少需要打断式处理）。

## 3. 控件与交互

### 3.1 OSC 自动隐藏状态机

```
可见 ──(2500ms 无输入 且 未钉住)──▶ 隐藏
隐藏 ──(任意按键/鼠标移动/滚轮/单击)──▶ 可见（重置计时）
钉住条件（任一满足即不隐藏）：
  指针位于控制栏或其弹出菜单上 / seek 条拖动中 / 任一浮层打开 / 播放器处于 Paused / Stopped / Error
```

- IINA 取 2.5s 默认；暂停钉住采纳自 vidstack（暂停时用户正要操作，隐藏是反直觉的）。
- 拖动 seek 期间不提交，只显示预览时间；`IsItemDeactivated`（松手）才调 `Player::seek`——FFmpeg 后端的 seek 由解码线程异步执行，拖动中连续提交既浪费也抖动（vidstack scrub-preview 同语义）。

### 3.2 快捷键表（v0.1 固定，v0.2 可配置）

| 键 | 动作 | 来源 |
|---|---|---|
| `Space` / `K` | 播放/暂停切换 | mpv + YouTube |
| `←` / `→` | seek ∓5s | mpv 默认 |
| `Shift+←` / `Shift+→` | seek ∓1s（精确） | mpv 默认 |
| `PgUp` / `PgDn` | seek ∓60s | mpv UP/DOWN 语义移到 Page 键 |
| `Home` | 回到开头 | mpv |
| `0`–`9` | 跳到 0%–90% | plyr 语义（mpv 的 1–8 画质调整不直觉） |
| `↑` / `↓` | 音量 ±5% | YouTube/vidstack/plyr |
| `M` | 静音切换 | mpv/plyr 共识 |
| `F` | 全屏切换 | mpv/plyr 共识 |
| `A` / `C` | 循环切换音轨 / 字幕轨（含关闭） | mpv `#`/`j` 的可记忆化 |
| `I` / `R` / `H` | 信息 / 最近打开 / 快捷键帮助 | IINA 信息面板习惯 |
| `Esc` | 全屏时退出全屏；否则退出程序 | mpv.net/vidstack 语义，兼容现有 CLI 测试契约 |

### 3.3 鼠标与滚轮

- 单击视频区：切换 OSC 显隐（IINA `singleClickAction`）；双击：全屏切换（mpv）。判定用 500ms 内二次单击（SDL 双击事件语义）。
- 垂直滚轮：音量 ±5%（IINA 默认）；水平滚轮或 `Shift+滚轮`：seek ±5s。
- 拖放文件到窗口：打开该文件（SDL_DROPFILE）。
- seek/音量条拖动：实时预览数值（时间 / 百分比），seek 松手提交。

### 3.4 悬停预览

- v0.1：seek 条悬停显示目标时间 tooltip（ImGui 原生 tooltip）。拖动中在条上方跟随显示预览时间与"松手提交"的视觉暗示（颜色区分已缓存/未提交：预览态用强调色）。
- v0.2（边界）：悬停缩略图（参照 IINA 120px 宽 / 500 张缓存与 thumbfast 的分离式缩略图服务架构——解码器在后台线程按需出帧，UI 只消费纹理）。

### 3.5 手势与迷你模式（v0.2 边界）

- 迷你模式：IINA 音乐模式形态（`autoSwitchToMusicMode` 默认 true），纯音频自动缩为小窗 + 封面；等字幕渲染/播放列表之后做。
- 触控板手势（双指 seek/音量）与键盘可配置化同期。

## 4. 视觉风格

- **暗色为默认且唯一主题**（§1.6-7）。色板（设计令牌化，集中在 `ui_theme` 常量区，不散落）：
  - 背景/留黑 `#0A0B0E`；面板 `rgba(16,18,24,0.92)`（半透明压在视频上）；描边 `rgba(255,255,255,0.08)`。
  - 文本主 `#E8EAF0` / 次 `rgba(232,234,240,0.55)`；强调（进度、选中、悬停）`#4C8DFF`；危险/错误 `#FF5D5D`。
  - 对比度：文本主对背景 > 12:1，次文本 > 4.5:1（WCAG AA）。
- **布局栅格**：4px 基准网格；OSC 高 ~52px、内边距 8×6、控件间距 8；浮层宽上限 720px、行高 24。视频区永远信箱式等比（黑边即背景色）。
- **动效**：OSC 显隐与 Toast 用 160ms 透明度过渡（帧循环内线性插值，`disableAnimations` 不设开关，v0.1 常开）；无位移类动画（视频上方的 UI 不遮挡观看是第一原则）。暂停/缓冲指示 1s 淡出。
- **字体**：优先加载系统 UI 字体（每平台候选路径列表：Linux `Noto Sans CJK`/`DejaVu Sans`、macOS `PingFang`、Windows `Microsoft YaHei`），失败回退 ImGui 内嵌位图字体（仅 ASCII）——不捆绑字体文件，保持零新增分发物。

## 5. 技术选型

### 5.1 候选对比

| 维度 | SDL2 自绘 | **Dear ImGui（选定）** | Qt6 Widgets/QML | Web 壳（CEF/WebView） |
|---|---|---|---|---|
| 依赖增量 | 零 | 一个头文件库（FetchContent 拉取，MIT） | 数百 MB 运行时/工具链 | 最大（CEF ~200MB） |
| 跨平台一致性 | 高 | 高（自绘到 SDL_Renderer） | 高（原生外观各异） | 高 |
| 与现有核心契合 | 已在用（视频纹理路径） | **复用现有 SDL2 窗口/渲染器/事件循环，视频纹理→UI 叠加零冲突** | 需要并行事件循环/音频输出整合 | 需要 IPC/本地服务桥 |
| 现代 UI 成本 | 极高（每个控件手写；文本需 SDL_ttf 或位图字体） | **低（滑条/组合框/菜单/tooltip/主题开箱即用，默认暗色）** | 中（QML 声明式，但绑定层代码量大） | 中（前端栈另起） |
| 性能（视频上叠加 UI） | 手工控制 | 立即模式每帧重绘，视频路径不受影响 | 场景图开销独立 | 合成层多一跳 |
| 无障碍 | 全手写 | 弱（无屏幕阅读器协议）——用**键盘全覆盖**缓解 | **强（原生 a11y）** | 强（ARIA） |
| 测试可驱动性 | 事件注入直白 | 事件注入直白（SDL 层）+ 逻辑层可纯单测 | 需要 QTest/ Accessibility 桥 | 需要 CDP/自动化桥 |
| 许可证 | LGPL（已合规使用） | MIT | LGPL/商业双轨 | 多变（CEF BSD + Chromium 一堆） |

### 5.2 结论与风险

**v0.1 选 Dear ImGui（SDL2 + SDL_Renderer2 后端）**，理由：

1. 唯一能**零重构复用现有链路**的选项：SDL2 窗口、YUV 纹理上屏、事件循环全部保留，ImGui 以 `imgui_impl_sdlrenderer2` 后端在同一渲染器上叠加绘制，视频帧与 UI 无合成冲突。
2. 单一轻依赖（MIT），FetchContent 钉版本拉取，不进分发物，符合"依赖选型守住许可证边界"的原则（docs/licensing.md）。
3. 控件库即取即用：滑条（seek/音量）、组合菜单（倍速/音轨/字幕）、tooltip（悬停时间）、主题令牌，把工作量压在交互语义而非画像素上。

**明确记录的取舍**：

- **无障碍弱**是 ImGui 的硬伤（无平台 a11y 协议）。缓解：全功能键盘化（§3.2 全表可盲操作）、焦点可见、对比度达标；**当项目需要真实屏幕阅读器支持时迁移 Qt**——核心/UI 已分层（§1.5），迁移不动 `soar_core`。
- **立即模式每帧重绘**：与视频帧率同频（≤60fps），相比保留模式多耗的 CPU 在可接受范围；后续可加"无输入且无新帧时跳过呈现"的脏矩形节流（v0.2 优化项，不阻塞）。
- **Qt 是 v1.0 的备选而非 v0.1 的选择**：播放列表管理、设置面板、真无障碍到来时，若立即模式的代码组织开始别扭，整体迁移到 Qt6 QML（IINA 式桌面形态），届时 UI 层只重写视图，事件模型与 Player 门面不变。
- **Web 壳否决**：为本地媒体播放引入浏览器运行时（体积、启动、本地文件安全策略、IPC 桥）全是负收益，其优势（Web 生态控件）对本项目无用武之地。

### 5.3 代码结构（实现的落点）

```
src/app/main.cpp            # CLI 解析、后端选择、Player 装配（不变）
src/app/ui/ui_state.{h,cpp} # 纯逻辑（无 SDL/ImGui 依赖）：自动隐藏状态机、
                            #   RecentStore（MRU×15、原子写）、Toast、时间格式化
                            # → 静态库 soar_app_ui，供 app 与单测共享
src/app/ui/player_window.{h,cpp} # SDL2 窗口 + ImGui OSC/浮层 + 键鼠映射
                            #   （SOAR_WITH_SDL2 && SOAR_WITH_IMGUI 才编译）
tests/test_ui_state.cpp     # ui_state 的 doctest 单测（soar_ui_tests）
tests/test_cli.cpp          # 新增 Xvfb + XTEST 键盘驱动用例（沿用现有注入器模式）
```

CMake：`SOAR_ENABLE_IMGUI`（默认 ON）取 ImGui v1.91.9b（含 `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` 后端）。SDL 下限取 **2.0.18**：后端自身要求 2.0.17（`SDL_RenderGeometry`），而窗口层的按键重复抑制用了 2.0.18 才有的 `SDL_KeyboardEvent::repeat`，因此 `find_package(SDL2 2.0.18 CONFIG QUIET)` 把"太旧"也并入降级路径。拉取方式是**显式 `git clone --depth 1` 到 `<build>/_deps/imgui-src`**而非 `FetchContent`：后者拉不到就直接让 configure 失败，与本项目"可选能力缺失即降级"的约定（SDL2/FFmpeg 同款）冲突；显式克隆把失败变成一次降级提示，已存在的目录直接复用。ImGui 源码在构建目录，不进仓库、不进 gcovr 统计口径（`--filter src --filter include` 天然排除）。CI 的 `no-imgui` job 用 `-DSOAR_ENABLE_IMGUI=OFF` 构建并跑窗口用例，钉住这条降级路径。运行时 `io.IniFilename` 置 `nullptr`：所有窗口都是 `NoSavedSettings`，让 ImGui 往用户工作目录写 `imgui.ini` 没有收益。

## 6. v0.1 验收清单

- [x] 打开（CLI/拖放/最近列表）、播放、暂停、停止、seek（条 + 快捷键 + 百分比）
- [x] 音量/静音、倍速（0.5–2.0 六档）
- [x] 音轨/字幕轨菜单选择与 `A`/`C` 循环（含关闭字幕）
- [x] 信息浮层（媒体信息全量）+ 缓冲指示 + 错误 Toast
- [x] OSC 自动隐藏/钉住、单击切换、双击全屏、滚轮音量/seek
- [x] 最近打开：记录（MRU×15）、重开、持久化
- [x] 快捷键帮助浮层；Esc 语义（全屏先退出）
- [x] 键盘驱动全部功能的 Xvfb 集成用例 + ui_state 纯逻辑单测全绿
- [x] 覆盖率门禁不回退（新代码按既有口径统计，门禁随水位上抬）

覆盖口径见 [docs/coverage-notes.md](coverage-notes.md) §3.6：`player_window.cpp` 里的不可达项是"必须注入失败才能到达"的防御分支与字体候选路径，不是漏测的交互。

## 7. v0.2+ 边界（本版不做，防扩散）

播放列表页（最近列表升级为侧栏队列）、文件对话框、seek 悬停缩略图（120px/500 张缓存规格起步）、迷你音乐模式、设置面板、快捷键可配置、字幕渲染接入后的字幕菜单实效化、多主题。
