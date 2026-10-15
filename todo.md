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
- [ ] 播放键改 64px 圆形 accent 钮（原型 desktop-main.html:255）
- [ ] 圆角/字号令牌对齐 v2（`--r-panel 10px`/`--r-ctl 7px`、11px 字号地板）
- [ ] 焦点可见态 + reduced-motion 开关（v2 无障碍承诺，当前无焦点环）
- [ ] 顶栏——待用户拍板：(a) 保持无顶栏并改原型（推荐）/ (b) 加顶栏并改 ui-design.md / (c) 混合

> 依据 `docs/prototype-alignment-audit.md` §5。已复核**不适用**的两条（勿再立项）：
> 状态徽标（已实现于 `drawChips()`，`player_window.cpp:2199-2225`）、分辨率 pill
> （MK_R2A 已代决：`MediaInfo` 无宽高、核心接口冻结）。

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

## 安装包格式补全（2026-10-04）

- [x] deb/rpm/AppImage/setup.exe + 依赖 DLL 闭包 + SHA256 清单进 nightly（ed8b34c、e1990f3）

## 挂账

- [x] 挂账拍板：Welcome/DPI/亮色复审关闭，鸿蒙路线留用户拍板（294d3dc）
- [ ] BUGS #3 Windows 真机复测：soar 侧行为 CI 已覆盖（无参空启动存活/关联注册/空窗入口/argv 传播），剩 OS 壳行为（Explorer 发起的拉起、真文件对话框点选）待真机（2026-10-15 收窄口径）
- [x] BUGS #4 watchdog（退出路径 stop/join 次序）：两处无界等待修（c416163，stop rewind 2s 预算 + 音频背压可中断）；根因未经本地复现坐实，探针留防
- [ ] 真实电视设备矩阵（硬件侧）：夹具侧 SSDP 应答器已收编（2026-10-15，`--cast=auto` 发现端到端 + 浮层发现行走查），厂商 SSDP 互操作面（MX 延迟/组播路由/应答形态/跨子网）待真机
