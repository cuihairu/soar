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

; 资源管理器「打开方式 → Soar」注册（BUGS.md #3 收口 ③）。只注册
; OpenWithProgids 候选、不写 .ext 默认值——不抢用户的默认播放器，由
; 用户在资源管理器里自己设为默认。HKA = 管理员装到 HKLM、按用户装到
; HKCU，与 PrivilegesRequired=admin 的机器级安装对齐。命令行走 soarw.exe
; （GUI 前端），路径参数经 WinMain→CommandLineToArgvW→UTF-8 进原有播放
; 流程；卸载时 ProgId 整键删除、各扩展只删自己的候选值。
Root: HKA; Subkey: "Software\Classes\Soar.Media"; ValueType: string; ValueData: "Soar media file"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Soar.Media\DefaultIcon"; ValueType: string; ValueData: "{app}\soar.ico,0"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Soar.Media\shell\open\command"; ValueType: string; ValueData: """{app}\soarw.exe"" ""%1"""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.flv\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.m2ts\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.mp3\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.m4a\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.aac\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.flac\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.wav\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.ogg\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.opus\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.m3u8\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.mpd\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\.torrent\OpenWithProgids"; ValueType: string; ValueName: "Soar.Media"; ValueData: ""; Flags: uninsdeletevalue

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
