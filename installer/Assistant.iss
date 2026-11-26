; Assistant 安装包（Inno Setup 6）
;
; 设计原则：
;   * per-user 安装到 %LOCALAPPDATA%\Programs\Assistant（不需要管理员）；
;   * 程序目录与用户数据分离：数据在 %LOCALAPPDATA%\AssistantTeam\Assistant，安装/卸载都不碰；
;   * 固定 AppId：以后覆盖安装即为升级；
;   * 编译时用 /D 传入版本与 staging 目录，脚本本身不含绝对路径。
;
; 编译：
;   ISCC.exe /DMyAppVersion=0.3.0 ^
;            /DMySourceDir=..\dist\Assistant-0.3.0 ^
;            /DMyOutputDir=..\dist ^
;            installer\Assistant.iss

#ifndef MyAppVersion
  #define MyAppVersion "0.3.0"
#endif
#ifndef MySourceDir
  #define MySourceDir "..\dist\Assistant-" + MyAppVersion
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\dist"
#endif
#ifndef MyAppId
  #define MyAppId "{{8F3A4C1E-5B2D-4E77-9A61-2C0D7B9E4F10}"
#endif
#ifndef MyPortable
  ; 1 = 测试/绿色模式：不写卸载键、不建快捷方式（沙箱里也能装）
  #define MyPortable "0"
#endif
#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "Assistant-" + MyAppVersion + "-setup"
#endif

[Setup]
AppId={#MyAppId}
AppName=Assistant
AppVersion={#MyAppVersion}
AppVerName=Assistant {#MyAppVersion}
AppPublisher=AssistantTeam
DefaultDirName={localappdata}\Programs\Assistant
DefaultGroupName=Assistant
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Assistant.exe
UninstallDisplayName=Assistant
CloseApplications=yes
RestartApplications=no
; 安装包不写 HKLM、不装服务；数据在 LocalAppData，卸载保留。
#if MyPortable == "1"
CreateUninstallRegKey=no
Uninstallable=no
#endif

[Languages]
; Inno 6 的 per-user 安装不带中文语言包；界面用英文，任务/快捷方式文案仍是中文。
; 需要中文向导时把 ChineseSimplified.isl 放进安装器的 Languages\ 再改回 chinesesimplified。
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

#if MyPortable != "1"
[Icons]
Name: "{autoprograms}\Assistant"; Filename: "{app}\Assistant.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Assistant"; Filename: "{app}\Assistant.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Assistant.exe"; Description: "启动 Assistant"; Flags: nowait postinstall skipifsilent
#endif

[Code]
// 覆盖安装前结束正在运行的实例；数据目录不受影响。
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/IM Assistant.exe /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/IM file_guard.exe /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
