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
- [x] 并案收口：安装包里带上依赖 DLL（闭包捆绑，装完就能跑——
      缺 DLL 缺陷见 BUGS.md #1，黑框闪退见 BUGS.md #2）

### 如实说明

- 用户消息预设 nightly 已有 AppImage，实际现状只有 zip（此前从未
  打过 AppImage）。本批交付点名的 deb/rpm/setup.exe；AppImage 缺口
  如实回报，未擅自扩面。若后续要做，单开一批。
- Linux deb/rpm 不声明 Depends（运行库随包在 /opt/soar/lib，自包含
  口径与 zip 一致；目标机只需 glibc/libstdc++ 底线，同包内
  PLATFORM-NOTES.txt）。
