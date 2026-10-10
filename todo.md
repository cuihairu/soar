# todo

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
