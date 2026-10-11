# todo

## 快捷键可配置批（2026-10-11 登记，ui-design.md §3.2「可配置留待后续」收口）

范围：键位映射组件（默认表 + 配置文件重绑 + 全局开关）、窗口接线、帮助浮层
实时键位列、CLI 开关、X11 走查用例。不在本批：设置面板 GUI（仍不做，手工
编辑配置文件即本批交付形态）、按键录制器。

- [x] **S1 键位映射组件**：`src/app/ui/shortcuts.{h,cpp}`（soar_app_ui 目标，
      纯逻辑无 SDL，同 ui_state/dir_scan/library 契约；自带 `kKey*` 键码表，
      player_window.cpp 71 条 `static_assert` 对 SDL 逐值钉死——漂移编译期
      失败）。`ShortcutMap`：出厂默认表（即 §3.2 表）、行式配置重绑（行替换
      动作默认绑定、单字符区间、shift/ctrl/alt 前缀、空值解绑、`shortcuts
      = off` 全局开关）、查找规则（无修饰绑定无视修饰键=复刻原 switch；带
      修饰绑定同键优先；同键两行后行胜出=后面的行拿走键）、typo 行跳过
      不解绑。测试 `tests/test_shortcuts.cpp`（16 用例）+ `soar_shortcut_tests`。
- [x] **S2 窗口接线**：`handleEvent` 硬编码 switch 换 `bindingFor()` 查表
      （34 动作全覆盖，行为逐键等价——percent_seek 用命中序号×10%，自定义
      键集按序映射）；帮助浮层键位列改由当前绑定实时渲染（默认配置下与
      原静态表逐字节一致，X11 标签哈希用例不受扰）；`--shortcuts=FILE` /
      `--no-shortcuts` CLI 开关（后者优先级高于文件内 `shortcuts = on`）。
- [x] **S3 X11 走查用例**：注入脚本加 `rebind` 模式（旧键 4 连按窗口必
      留存 → rc=7；新键风暴 20s 内退出 → rc=0），用例
      「windowed run honors a rebound quit key from the shortcuts config」
      （`quit = x`：q 留存、x 退出；SDL_CreateRenderer 失败环境跳过）。
      本地 build-cov3 实跑 7 个无后端 X11 用例全绿（含 HUD 走查/库浮层/
      续播）。排障插曲：手工探针曾误用 build-system 树的二进制——该树
      `SOAR_ENABLE_IMGUI=OFF`（无 ImGui 的裸回退循环只硬编码 Escape），
      字母键无反应是回退路径使然而非映射缺陷；套件树 build-cov3
      IMGUI=ON，用例真跑即过。
- [ ] **S4 全量门禁 + 提交推送**（本地 fullgate 过线 → rebase → push →
      CI 七腿绿）。

### 如实说明

- 默认行为零变化是验收线：出厂默认表逐键复刻原硬编码 switch（含
  shift+q 退出、shift+← 1s、ctrl+o 文件夹），帮助浮层默认文案逐字节不变。
- 键码表两处错值（DELETE 127 ASCII 段、MENU 0x40000076）由 S2 的
  static_assert 网络编译期坐实后修正——钉死网络正是为这类漂移而设。
- 重绑只接管应用级快捷键：浮层内部的按键（如播放队列浮层的 `x` 删行）
  不走映射表，把某个动作绑到那个键会两处都触发——属用户自选冲突，与
  设置面板的按键录制器同类问题，本批不做消解。

## 媒体库批（2026-10-10 登记，mvp.md §3「媒体库与刮削、历史/同步、多端一致性」）

范围：媒体库核心（监视文件夹 + 扫描 + 持久化）、历史/断点续播、窗口库浮层。
不在本批：HDR/真机硬解（卡硬件）、投屏、插件体系、ASR 语音翻译（用户令排除）；
网络刮削（TMDB 等外部服务）与多端同步（需传输协议决策）留后续批，本批刮削
只覆盖离线 NFO 边车（M4，视余量）。

- [x] **M1 库组件**：`src/app/ui/library.{h,cpp}`（soar_app_ui 目标，纯逻辑无
      SDL/ImGui，同 ui_state/dir_scan 契约）——`MediaLibrary`：监视文件夹
      增删、递归扫描（复用 `listMediaFiles` + stat，合并/剪枝语义）、
      历史 API（recordOpen/recordProgress/clearPosition/removeEntry）、
      原子持久化（tmp+rename，行格式，容错解析）、`defaultLibraryPath()`
      （XDG state 同 recent.txt）、`resumePositionMs()` 纯策略函数。
      测试 `tests/test_library.cpp` + `soar_library_tests` 目标。
- [x] **M2 窗口历史接线**：`WindowUiConfig::library` 指针（main.cpp 持有实例，
      headless 不碰状态——沿用 RecentStore 既有契约）；构造器 load + 初始源
      断点续播 seek；recordOpen 接线；播放中 5s 节流 recordProgress +
      Ended 清位；析构器最终记录+保存；openSource/openPlaylistEntry 续播
      seek+toast；torrent 桥 URL 跳过历史；`--no-resume` CLI 开关。
      脚本化窗口用例：预置 library.txt 断言续播位置事件。
- [x] **M3 库浮层 UI**：`b` 键 Overlay::Library——监视文件夹区（增：复用
      DirPick「监视此文件夹」模式；删）、Rescan、条目列表（名/时长/续播
      标记）、点击即播（带续播 seek）；帮助浮层补 `b` 行；ui-design.md
      §1.6 键位表补 `B 媒体库`。脚本化用例：浮层开+条目点击+监视文件夹流。
- [x] **M4（视余量）离线 NFO 刮削**：`<名>.nfo` 边车 title/plot 解析
      （扫描时读取，内存缓存不落持久化——边车文件是唯一真相），库浮层行
      标题优先显示刮削名、悬停提示带 plot。网络刮削与多端同步另立批。

### 如实说明（本批边界）

- 历史记录只走窗口会话（headless 保持无状态，与 RecentStore 既有契约一致；
  所有窗口用例已按 XDG_STATE_HOME 隔离，库文件同路径同隔离）。
- 续播策略：position ≥ 5s 且 duration 已知且 position ≤ duration−5s 才续播；
  Ended 清位（看完即忘）。torrent 桥 URL 每会话换端口，不进历史。
- 扫描语义：递归；条目按路径排序；监视文件夹内消失的文件被剪枝，非监视
  路径（直接打开的 URL/文件）的条目永不被扫描剪枝。
- 存储键分隔符一律归一为 '/'（Windows 折叠 '\'，POSIX 恒等——'\' 在
  POSIX 可以是文件名的一部分）：argv/对话框/扫描/手工编辑四种来源同形，
  同一文件不因写法差异裂成两行历史（2026-10-11 收口，Windows CI 实红
  test_library 分隔符断言坐实的库内缺口，非仅断言放宽）。

## 安装包格式补全（2026-10-04 登记，本批收口）

用户需求：soar 安装包缺格式——

- [x] ① Linux 补 **.deb 和 .rpm**（同二进制一起进 nightly，元数据/
      图标/.desktop 齐）——`scripts/package-linux-pkg.sh` 从同源 zip
      二次打包（单一事实源），/opt/soar 布局 + /usr/bin/soar +
      .desktop + hicolor 图标；CI 真装走查（dpkg -i → --version →
      dpkg -r；rpm -ivh → --version → rpm -e）
- [x] ② Windows 补**安装包 setup.exe**（Inno Setup，参照
      falcon/memex 形态：装目录/开始菜单/桌面图标/卸载器，窗口不带
      终端黑框——快捷方式指向 GUI 子系统的 soarw.exe），
      `packaging/windows/soar.iss`；zip 照旧保留
- [x] ③ 全部进滚动 nightly Release + SHA256 清单
      （SHA256SUMS.txt 随 Release 发布）
- [x] ④ Linux 补 **AppImage**（上批如实回报的缺口，2026-10-10 单开
      一批收口）——`scripts/package-linux-appimage.sh` 从同源 zip
      二次打包（单一事实源，AppDir 根 = zip 内容原样，RUNPATH 零
      改写），AppImageKit continuous runtime 前置拼接 + squashfs
      （gzip，最大兼容面）；CI 走查三启动面：FUSE2 直跑（
      libfuse2t64）+ APPIMAGE_EXTRACT_AND_RUN + --appimage-extract，
      另进裸容器冒烟（无 FUSE，验证包自含 runtime+载荷）；
      x64/arm64 双架构进 nightly 与 SHA256 清单
- [x] 并案收口：安装包里带上依赖 DLL（闭包捆绑，装完就能跑——
      缺 DLL 缺陷见 BUGS.md #1，黑框闪退见 BUGS.md #2）

### 如实说明

- ①②③ 交付时 nightly 实际只有 zip（AppImage 从未打过），缺口如实
  回报未擅自扩面；④ AppImage 于 2026-10-10 按上批约定单开一批补齐。
- Linux deb/rpm 不声明 Depends（运行库随包在 /opt/soar/lib，自包含
  口径与 zip 一致；目标机只需 glibc/libstdc++ 底线，同包内
  PLATFORM-NOTES.txt）。AppImage 载荷与 zip 同源，底线相同；AppImage
  直跑需系统 FUSE2（libfuse.so.2，缺则走 APPIMAGE_EXTRACT_AND_RUN
  免安装运行面）。

## 挂账项拍板（2026-10-10）

- **Welcome 页 / DPI 三档 / 亮色四页复审：不追（关闭）**。DPI 125%/150%
  需真机（runner 桌面仅 100%，RDP/多会话接管风险评估后已放弃盲写）；
  Welcome 页为 Inno 内置英文页（限制已登记，自定义文案仅 Tasks/Run
  两短行已实现）；亮色四页 v2.1 已随 e5f9bf6 发货并有截图证据链，
  无新重设计待复审。
- **鸿蒙内核路线：维持留用户拍板**（055ecdb 记录 + README 登记 + 一律
  后置令继续生效），本轮不改码。
- BUGS #3（Windows 真机复测）/ BUGS #4（watchdog 挂账，根因位
  player_window.cpp 退出路径 stop/join 次序）维持挂账，留待专门批。
