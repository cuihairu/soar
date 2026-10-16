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
- [ ] docs 双语缺口补齐：`mvp` 与 `ui-design` 缺英文裸名版（本轮只接 VitePress locales 路由，不翻译）

## 挂账

- [x] 挂账拍板：Welcome/DPI/亮色复审关闭，鸿蒙路线留用户拍板（294d3dc）
- [ ] BUGS #3 Windows 真机复测：soar 侧行为 CI 已覆盖（无参空启动存活/关联注册/空窗入口/argv 传播），剩 OS 壳行为（Explorer 发起的拉起、真文件对话框点选）待真机（2026-10-15 收窄口径）
- [x] BUGS #4 watchdog（退出路径 stop/join 次序）：两处无界等待修（c416163，stop rewind 2s 预算 + 音频背压可中断）；根因未经本地复现坐实，探针留防
- [ ] 真实电视设备矩阵（硬件侧）：夹具侧 SSDP 应答器已收编（2026-10-15，`--cast=auto` 发现端到端 + 浮层发现行走查），厂商 SSDP 互操作面（MX 延迟/组播路由/应答形态/跨子网）待真机
