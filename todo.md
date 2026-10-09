# todo

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
