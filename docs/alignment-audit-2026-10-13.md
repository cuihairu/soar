# 原型对齐验收（2026-10-13）

触发：todo 28/28 全勾但无「原型对齐」条目、无截图证据。本验收以运行截图为据，
逐屏三档结论（已对上 / 有差异 / 未实现）。证据：`evidence/alignment-2026-10-13/`
（Xvfb :116 独占、`SOAR_UI_BITMAP_FONT=1`、build-cov3 真二进制、键盘驱动、暂停态
固定帧；library 屏用种子 `library.txt` 验证续播进度读出）。

## 原型清单

| # | 原型 | 版本 | 实现对照证据 |
|---|------|------|--------------|
| 1 | `prototypes/redesign/desktop-main-{light,dark}.png` | v2.1 | `desktop-main.png`（播放+OSC 暂停态）、`empty-first-run.png` |
| 2 | `prototypes/redesign/playlist-{light,dark}.png` | v2.1 | `playlist.png` |
| 3 | `prototypes/redesign/library-{light,dark}.png` | v2.1 | `library.png` |
| 4 | `prototypes/redesign/settings-{light,dark}.png` | v2.1 | 无（未实现） |
| 5 | `prototypes/translate-panel.png` | v2.0 | 无（未实现） |

## 逐屏结论

### 1. desktop-main（主播放窗）—— 有差异

对上：视频区 + 底部 OSC（seek 条一行 + 控件一行两行式）、toast（Paused）、字幕
渲染、全屏、播放/暂停/停止、音量滑条、倍速、音轨/字幕轨切换（原型放在设置页，
实现放 OSC，功能覆盖且更强）、A-B 循环（L）、字幕开关（V）、截图（S）、
上一首/下一首（N/Shift+N）——功能面全在，键盘路径完整（`help.png` 为证）。

差异（逐条）：
1. **无顶栏**：原型有 logo + 标题 + 信息/最近/帮助/全屏四图标；实现无顶栏，
   标题信息在 OS 窗口标题栏（`soar - <文件名>`，player_window.cpp:788），
   四图标功能由 OSC Info/Full 按钮 + R/H 快捷键覆盖。
2. **无状态 pill**：原型顶部「正在播放 时间 P2P 45%」；实现无（等价信息在
   Info 浮层与 OSC 时间）。
3. **OSC 控件形态**：原型 prev/播放/next 大圆钮 + 剩余时间 `-00:02:14` +
   画质 pill（1080p 60）+ A-B/CC/截图按钮；实现为 Play/Stop + 时间文本 +
   Mute/滑条 + 1×/Audio/Sub 下拉 + Sub/Info/Full 按钮——A-B/CC/截图/prev/next
   只有键盘路径（L/S/V/N/Shift+N，`help.png` 为证），OSC 无按钮；无剩余
   时间、无画质 pill。

对齐补记（初版误判撤回）：**OSC 2.5s 自动隐藏已实现**——`HudVisibility`
（ui_state.cpp:91，kHideDelay=2500ms 即原型值/IINA 默认），悬停/拖动/浮层/
暂停四钉住 + 单击显隐 Force 切换 + 0.16s 淡出（player_window.cpp:835）。
初版截图全部摄于暂停态，暂停钉住 OSC，故误判「常显」；播放中静置的
`osc-playing.png`/`osc-hidden.png` 证据对因六树门禁占用构建树暂缓补拍，
入 evidence/ 后在 OSC 批提交补账。剩余小差异：**鼠标移动不算输入**
（SDL_MOUSEMOTION 无处理，仅 key/wheel/click 唤醒；原型「无输入」含移动），
已登记 todo。

### 2. playlist（播放列表）—— 有差异

对上：列表 + 当前行高亮（≻ 标记 + 高亮行）、Loop 三态（off/all/one 循环
按钮，player_window.cpp:2442；原型为分段控件 关/全部/单曲）、Shuffle 开关、
行移除（×）、打开文件夹、目录分组树（v0.2.1，原型没有的增强）。

差异（逐条）：
1. **形态**：原型是整页 + 左侧栏（logo + 四功能图标）；实现是居中浮层
   （单窗浮层 IA，ui-design §2 如此规定）。
2. **行信息**：原型有缩略图 + 序号 + 时长 + 格式行（codec · size）；实现只有
   文件名 + ×。
3. **无「添加文件」按钮**：O 快捷键可开文件，浮层只有「Open folder...」。
4. **无行 hover 动作**：原型 hover 浮现「下一首播放 / 移除」；实现只有常显 ×。
5. **无清空队列按钮**（原型右上角垃圾桶）。
6. **无底部 mini-player**（原型底部当前项 + 进度条）。

### 3. library（媒体库）—— 有差异（结构性最大）

对上：条目带续播进度（实现 `(at 00:45) 01:02` 内联 vs 原型「已下载 45%」
状态行——种子数据验证读出正确）、文件夹扫描入库（watched folders）、
点击条目播放、Add folder/Rescan 管理入口。

差异（逐条）：
1. **无分类侧栏**：原型左侧 全部/最近加入/续播/已看完 + 本地文件/网络源 +
   计数；实现无分类（数据层已有 position/duration/last_played/play_count
   字段，分类是视图层工作）。
2. **无卡片网格**：原型缩略图卡片 + 徽标（P2P/HLS/.torrent/续播）；实现纯
   文本列表。
3. **无搜索框**：原型右上「搜索媒体…」+ `/` 聚焦；实现无。
4. **库视图范围**：原型库含网络源/P2P 条目（徽标区分来源）；实现列表只含
   本地扫描文件。

### 4. settings（设置页）—— 未实现

ui-design §2 明文「设置：v0.1 无设置页……v0.2 再做成面板」。原型 5 组导航
（通用/播放/字幕/网络与下载/关于）与字幕页（字体/字号/时长对齐/渲染风格/
外挂目录/自动下载/端点配置）均无 UI。部分能力散落他处：字幕轨道选择在
「Subtitle Settings」浮层（OSC Sub 按钮，`help.png` 可见 H 帮助里无设置项）、
倍速/音量在 OSC。

### 5. translate-panel（字幕翻译浮层）—— 未实现

无 Translate overlay、无快捷键（ShortcutAction 枚举无此项）；后端有
export→translation 管线，但无面板 UI（端点状态/进度/逐行译文预览均未实现）。

## 实现超出原型的屏（原型未画，不参与对齐判定）

Info 浮层（`info.png`）、Recent 浮层、Help 浮层（`help.png`）、Cast 浮层
（`cast.png`）、空首启 drop 提示（`empty-first-run.png`）、playlist 目录分组
树——均为 ui-design §2 IA 定义或后续批（投屏/库）产物。

## 差异分类

### 需要实现（已登记 todo「原型对齐验收批」）

1. OSC 鼠标移动算输入（唤醒隐藏的 OSC；SDL_MOUSEMOTION→noteActivity）。
2. OSC 补齐 prev/next 按钮。
3. OSC 补齐 A-B 与截图按钮。
4. OSC 右侧剩余时间（-mm:ss）。
5. Library 分类侧栏（全部/最近加入/续播/已看完 + 计数）。
6. Library 搜索框（/ 聚焦）。
7. Playlist 添加文件按钮 + 清空队列按钮。

### 原型超前 / 建议改原型（不实现，建议原型侧标注里程碑）

1. **settings 页**：ui-design §2 已定 v0.1 不做、v0.2 做成面板——原型是 v0.2
   规划稿。建议 `prototypes/redesign/index.html` 标注「v0.2 规划稿」。
2. **translate-panel**：后端管线已在，UI 面板未实现——建议标注 v0.2+。
3. **顶栏 / 状态 pill**：等价信息在 OS 窗口标题 + Info 浮层 + OSC 时间，单窗
   浮层 IA 是 ui-design §2 定的——建议原型补一张浮层 IA 版桌面主屏，或把顶栏
   列为 v0.2+。
4. **缩略图 / 卡片网格 / P2P·HLS·.torrent 徽标**：数据层无缩略图缓存、库视图
   未区分来源类型——建议标注 v0.3 增强；todo 只收分类侧栏与搜索（数据已具备）。

## 结论

对上 0 屏 / 有差异 3 屏（desktop-main、playlist、library）/ 未实现 2 屏
（settings、translate-panel）。todo 全勾不等于对齐——本验收前确实无任何
对齐证据；现 7 屏有截图为证，7 项实现差异已登记 todo，4 项原型超前已给改稿
建议。desktop-main 的 OSC 自动隐藏经源码核实为已实现（初版凭暂停态截图
误判「常显」，已撤回）：暂停/悬停/拖动/浮层均钉住 OSC，验收取证必须区分
钉住态与计时态。
