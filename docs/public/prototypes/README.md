# 原型图投放目录

首页「界面预览」走马灯的原型图轨道（`docs/.vitepress/theme/Screenshots.vue`）
按以下**固定文件名**到本目录取图；文件未到时轮播里渲染占位卡，
**放入对应文件、下次构建部署后自动换成真图**（组件零改动）。

| 文件名 | 归属分组 | 内容 | 状态 |
|---|---|---|---|
| `redesign/desktop-main-dark.png` | 播放 | 桌面主界面（v2 定稿） | ✅ 已投放（v2.1 复审 2026-10-07） |
| `redesign/playlist-dark.png` | 播放 | 播放列表（v2 定稿） | ✅ 已投放 |
| `redesign/library-dark.png` | 播放 | 媒体库（v2 定稿） | ✅ 已投放 |
| `redesign/settings-dark.png` | 设置 | 设置面板（v2 定稿） | ✅ 已投放 |
| `mobile.png` | 移动端 | 移动端界面 | ⬚ 未到，占位卡保留（移动端不在桌面批范围） |

原型卡一律取 **v2 定稿**截图（`redesign/` 子目录，暗色版即定稿集）。**v1 的三张
PNG（`desktop-main.png` / `settings.png` / `translate-panel.png`）自 2026-10-15
起不再被走马灯引用**：文件保留在库（v1 页面仍在，v2 各页的「旧稿对照」链指向
它们），但首页不再展示；`translate-panel` 卡已删——v2 重设计未重出该页，翻译
功能在实现里走字幕菜单 `[translate]` 行，由该组的真实截图覆盖。

要求：PNG（或直接改 `Screenshots.vue` 里的扩展名），建议 16:9 比例
（如 960×540 / 1920×1080），与真实截图同框展示。

## 桌面原型五页（2026-10-07 批）

`desktop-main / playlist / settings / library / translate-panel` 五页 HTML
可视稿由仓库设计（1280×800 桌面视口，共享设计语言见 `assets/proto.css`），
**总览与逐页设计说明（布局/交互/状态）见同目录 `index.html`**
（站点部署后为 `/prototypes/index.html`；本文件与 PNG 均为静态原样拷贝，
不进 VitePress 页面路由，故不设站内链接）。
**该 v1 五页自 2026-10-15 起页头标「v1 旧稿（已废弃）」，v2 重设计见 `redesign/`**
（v2 已取代其视觉语言与 IA；v1 保留的唯一用途是 v2 各页的「旧稿对照」链）。
`playlist.png` / `library.png` 的 v1 版不在走马灯合同内，随批投放供审核与文档引用；
走马灯现取它们的 **v2 定稿**截图（见上表）。

如实说明：品牌位为中性占位（「S」字标示意位，非 logo 设计）；媒体库为
路线图 v1.0 前瞻设计（未开始实现），其余四页对应已落地能力。设计稿非产品
代码，页内注解（note/kbd）为评审说明。

后续若替换为外部设计稿（Figma / Sketch / 手绘皆可），按上表固定文件名
投放即可，组件零改动。
