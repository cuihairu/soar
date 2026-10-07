# 原型展示

> 桌面播放器 v2 重设计稿：主播放窗 / 播放列表 / 设置面板 / 媒体库四页，亮暗双主题。
> 以下是**设计稿**（HTML/CSS 原型，`docs/public/prototypes/` 下的静态页），不是产品代码；
> 实现落点见 [UI 设计](/ui-design)。品牌位为中性占位（纯色橙块 + S 字），非正式 logo。

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

![主播放窗 亮色](/prototypes/redesign/desktop-main-light.png)

外壳（顶栏 / 状态徽标 / 控制栏 / 注解浮层 / 进度槽）随主题用浅色；视频画布与其上的字幕属内容层，两主题恒黑。

### 主播放窗（暗色）

![主播放窗 暗色](/prototypes/redesign/desktop-main-dark.png)

### 设置面板（亮色）

![设置面板 亮色](/prototypes/redesign/settings-light.png)

### 媒体库（亮色）

![媒体库 亮色](/prototypes/redesign/library-light.png)

### 播放列表（亮色）

![播放列表 亮色](/prototypes/redesign/playlist-light.png)

## 决策记录

### v2.1 复审拍板（2026-10-07）

- **暗色四页通过，定稿。**
- **亮色版打回重做（本版完成）**：v2 亮色版名实不符，外壳仍整面深色。v2.1 把外壳全部主题化：顶栏、状态徽标与浮层 pill、注解浮层、OSC 控制栏、进度槽在亮色主题下都用真浅色；视频画布与其上的字幕属内容层两主题恒黑（真实播放器的正常形态，不算「假亮色」）。
- **小改两项**：主播放窗进度槽 4px 加粗到 6px（手柄 10px 到 12px，两主题同步）；左上注解两块合一（信息密度收敛；OSC 自动隐藏机制说明保留，一次不糊四块）。
- **复审范围**：重出亮色四页截图 + 主播放窗暗色截图（进度槽同步），其余暗色三页维持原稿。

### v2 重设计口径（2026-10-06）

- redesign-preserve：结构与信息架构不动，视觉语言收敛（渐变清零、单一信号橙强调，天蓝仅保留网络/P2P 语义、字号地板 11px、圆角成文，面板 10 / 控件 7 / 徽标 pill / 图标钮圆形、无限动画包 `prefers-reduced-motion` 守卫）。
- 设计令牌三层（原始层 / 语义层 / 组件层），组件不吃裸色值；对比度按 WCAG AA 校验（元信息档暗底 6.0:1、亮底 4.8:1）。
