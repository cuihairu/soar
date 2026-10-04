; Soar Windows 安装器（Inno Setup 6；falcon/memex 同形态）
;
; 产物：soar-setup-windows-x64.exe（固定资产名，nightly Release 直链
;       与 install.ps1 取件口径；解压即用面由 soar-windows-x64.zip 承担，
;       本安装器是「装目录/开始菜单/桌面图标/卸载器」的面）
;
; 构建（daily-build 的 Package (Windows) 步骤已把载荷暂存到 stage/ ——
;       exe + 依赖 DLL 闭包 + soar.ico，scripts/stage-windows-dlls.sh）：
;   ISCC.exe /DAppVersion=0.1.0 /DStageDir=<abs>\stage /O<abs> packaging\windows\soar.iss
;
; 要求：Inno Setup 6.3+（x64compatible 架构别名；GitHub windows-latest
;       镜像预装 6.x）。快捷方式一律指向 soarw.exe（GUI 子系统，双击
;       无控制台黑框，BUGS.md #2）。
; 验证：/VERYSILENT 静默安装 → 装后实跑 → 快捷方式断言 → 卸载走查
;       由 daily-build 的 Verify 步骤在真 Windows 上执行。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  ; 本地默认：仓库根相对路径（源文件路径相对本 iss 所在目录解析）
  #define StageDir "..\..\stage"
#endif

[Setup]
; AppId 固定 GUID：每日覆盖安装合并为同一卸载条目（升级安装语义）
AppId={{8E4C2D91-6B7A-4F5E-9C3A-1D2B5E7F9A03}
AppName=Soar
AppVersion={#AppVersion}
AppPublisher=soar nightly
; 装 Program Files（x64 位模式）——参照 falcon 形态，需要管理员提权
DefaultDirName={autopf}\Soar
DefaultGroupName=Soar
DisableProgramGroupPage=yes
SetupIconFile={#StageDir}\soar.ico
UninstallDisplayIcon={app}\soar.ico
OutputBaseFilename=soar-setup-windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 升级时目标程序在跑则先关（/VERYSILENT 下自动执行，不弹窗）
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; Flags: unchecked

[Files]
; 载荷 = stage 暂存的完整运行时（soar.exe / soarw.exe / 依赖 DLL 闭包
; / soar.ico / PLATFORM-NOTES.txt）——装完就能跑，缺 DLL 缺陷的并案
; 收口（BUGS.md #1）
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Soar"; Filename: "{app}\soarw.exe"; WorkingDir: "{app}"
; 卸载入口（开始菜单；控制面板/设置卸载条目由 Inno 注册表键自动生成）
Name: "{group}\Uninstall Soar"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Soar"; Filename: "{app}\soarw.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; 静默安装不自动启动（nightly 未签名，SmartScreen 首启交互留给用户）
Filename: "{app}\soarw.exe"; Description: "运行 Soar(&R)"; Flags: nowait postinstall skipifsilent unchecked
