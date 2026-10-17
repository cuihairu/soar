
# 原型展示

> 桌面播放器 v2 重设计稿：主播放窗 / 播放列表 / 设置面板 / 媒体库四页，亮暗双主题。
> 以下是**设计稿**（HTML/CSS 原型，`docs/public/prototypes/` 下的静态页），不是产品代码；
> 实现落点见 [UI 设计](/zh/ui-design)。品牌位为中性占位（纯色橙块 + S 字），非正式 logo。

## 在线预览

每页均支持 `?theme=light|dark` 参数或页内按钮切换主题。截图为 1280×800 静态帧。

| 页面 | 交互原型 | 暗色截图 | 亮色截图 |
| --- | --- | --- | --- |
| 主播放窗 | [打开](/prototypes/redesign/desktop-main.html) | [暗](/prototypes/redesign/desktop-main-dark.png) | [亮](/prototypes/redesign/desktop-main-light.png) |
| 播放列表 | [打开](/prototypes/redesign/playlist.html) | [暗](/prototypes/redesign/playlist-dark.png) | [亮](/prototypes/redesign/playlist-light.png) |
| 设置面板 | [打开](/prototypes/redesign/settings.html) | [暗](/prototypes/redesign/settings-dark.png) | [亮](/prototypes/redesign/settings-light.png) |
| 媒体库 | [打开](/prototypes/redesign/library.html) | [暗](/prototypes/redesign/library-dark.png) | [亮](/prototypes/redesign/library-light.png) |

完整说明（旧稿丑点清单、逐页改动理由）见 [重设计总览](/prototypes/redesign/index.html)。

## 关键帧

### 主播放窗（亮色）

<img src="/prototypes/redesign/desktop-main-light.png" alt="主播放窗 亮色" width="60%">

外壳（状态徽标 / 控制栏 / 注解浮层 / 进度槽）随主题用浅色；视频画布与其上的字幕属内容层，两主题恒黑。

> ⚠️ 主播放窗两版截图摄于 2026-10-07 定稿帧，**含顶栏**；2026-10-15 巡检代决已把顶栏
> 移除出 v2 稿（无顶栏为 v0.1 基准，见 [UI 设计](/zh/ui-design) §2），本仓无浏览器渲染
> 环境故未重摄，截图与现行原型页存在此一处已知差异（重摄前保留）。

### 主播放窗（暗色）

<img src="/prototypes/redesign/desktop-main-dark.png" alt="主播放窗 暗色" width="60%">

### 设置面板（亮色）

<img src="/prototypes/redesign/settings-light.png" alt="设置面板 亮色" width="60%">

### 媒体库（亮色）

<img src="/prototypes/redesign/library-light.png" alt="媒体库 亮色" width="60%">

### 播放列表（亮色）

<img src="/prototypes/redesign/playlist-light.png" alt="播放列表 亮色" width="60%">

## 决策记录

### v2.1 复审拍板（2026-10-07）

- **暗色四页通过，定稿。**
- **亮色版打回重做（本版完成）**：v2 亮色版名实不符，外壳仍整面深色。v2.1 把外壳全部主题化：顶栏、状态徽标与浮层 pill、注解浮层、OSC 控制栏、进度槽在亮色主题下都用真浅色；视频画布与其上的字幕属内容层两主题恒黑（真实播放器的正常形态，不算「假亮色」）。
- **小改两项**：主播放窗进度槽 4px 加粗到 6px（手柄 10px 到 12px，两主题同步）；左上注解两块合一（信息密度收敛；OSC 自动隐藏机制说明保留，一次不糊四块）。
- **复审范围**：重出亮色四页截图 + 主播放窗暗色截图（进度槽同步），其余暗色三页维持原稿。

### v2 重设计口径（2026-10-06）

- redesign-preserve：结构与信息架构不动，视觉语言收敛（渐变清零、单一信号橙强调，天蓝仅保留网络/P2P 语义、字号地板 11px、圆角成文，面板 10 / 控件 7 / 徽标 pill / 图标钮圆形、无限动画包 `prefers-reduced-motion` 守卫）。
- 设计令牌三层（原始层 / 语义层 / 组件层），组件不吃裸色值；对比度按 WCAG AA 校验（元信息档暗底 6.0:1、亮底 4.8:1）。

## 对齐验收（2026-10-13）

逐屏三档结论与截图证据见[对齐验收报告](/zh/alignment-audit-2026-10-13)：
有差异 3 屏（主播放窗 / 播放列表 / 媒体库），未实现 2 屏（设置面板、翻译浮层，
ui-design §2 定 v0.1 无设置页）。实现差异 7 项已登记 todo「原型对齐验收批」；
原型页里程碑标注已落地（2026-10-14，巡检）：`redesign/index.html` 增「原型超前项
里程碑标注」节（settings v0.2 规划稿、translate / 顶栏·状态 pill / OSC 画质 pill
v0.2+、媒体库缩略图·卡片网格·来源徽标 v0.3），`prototypes/translate-panel.html`
页头标 v0.2+。

## 文档站一致性（2026-10-15 收口）

首页「界面预览」的原型卡此前指向 **v1** 的 PNG（`prototypes/desktop-main.png` 等），
与本页只链 v2 自相矛盾——同一站点一处说 v2 定稿、一处放 v1 旧图。本次收口：

- **原型卡改指 v2 定稿截图**：`redesign/desktop-main-dark.png`、`redesign/settings-dark.png`，
  并补上此前从未在站内出现的 `redesign/playlist-dark.png`、`redesign/library-dark.png`
  （走「投放即生效」登记，文件已在库）。
- **删除 translate-panel 卡**：v2 重设计未重出该页（翻译功能在实现里走字幕菜单
  `[translate]` 行，已由该组两张真实截图覆盖）。
- **v1 原型集页头标废弃**：`prototypes/` 下六页（含 `index.html` 总览）的 docbar
  前置「v1 旧稿（已废弃，v2 见 …）」并链到对应 v2 页；v1 页保留不删——v2 各页的
  「旧稿对照」链仍指向它们。translate-panel 页注明 v2 未重出该页。
- 移动端卡（`mobile.png`）维持占位：移动壳尚无 UI 可对（见[对齐审计](/zh/prototype-alignment-audit) §6 平台分列），无 v2 稿可指。

## 顶栏移除（2026-10-15 巡检代决 a）

对齐审计 §5-P2-7 的三项由用户代决选 **(a) 保持无顶栏**：v2 原型主播放窗四帧的顶栏
（品牌位 + 居中片名 + I/R/H/F 工具钮）全部移除，理由沿用审计推荐——入口走 OSC + 快捷键
（I/R/H/F 均在 §3.2 合同）、片名走 OS 窗口标题，加顶栏引入常驻 chrome 面积与 §1.6-1
调研结论冲突。**顶栏与亮色版同属 v0.2 候选，非 v0.1 基准**（[UI 设计](/zh/ui-design) §2/§4
同步）。主播放窗两版截图为移除前定稿帧，未重摄（无渲染环境），已知差异见上文。
