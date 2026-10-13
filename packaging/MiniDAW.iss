#ifndef PayloadDir
  #error Supply PayloadDir.
#endif
#ifndef OutputDir
  #error Supply OutputDir.
#endif
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef AggregateVersion
  #define AggregateVersion "v20"
#endif

[Setup]
AppId={{8F3D560F-5DB0-4449-A0DD-EB789E172B25}
AppName=Mini DAW
AppVersion={#AppVersion}
AppPublisher=WanAn522z
AppPublisherURL=https://github.com/WanAn522z
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=Mini DAW x64 Setup
DefaultDirName={autopf}\Mini DAW
DefaultGroupName=Mini DAW
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.10240
PrivilegesRequired=admin
DisableProgramGroupPage=yes
DisableWelcomePage=no
OutputDir={#OutputDir}
OutputBaseFilename=MiniDAW-{#AppVersion}-x64-Setup
SetupIconFile=..\assets\minidaw.ico
UninstallDisplayIcon={app}\MiniDAW.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
InfoBeforeFile=INSTALL-zh-CN.txt

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.Full=推荐安装（Mini DAW + Aggregate Audio ASIO）
english.Full=Recommended (Mini DAW + Aggregate Audio ASIO)
chinesesimplified.MiniOnly=仅安装 Mini DAW（使用已有 ASIO 驱动）
english.MiniOnly=Mini DAW only (use an existing ASIO driver)
chinesesimplified.Custom=自定义安装
english.Custom=Custom installation
chinesesimplified.MainComponent=Mini DAW 主程序
english.MainComponent=Mini DAW application
chinesesimplified.DriverComponent=Aggregate Audio ASIO {#AggregateVersion}（可选，含配置程序）
english.DriverComponent=Aggregate Audio ASIO {#AggregateVersion} (optional, includes settings panel)
chinesesimplified.DesktopIcon=创建 Mini DAW 桌面快捷方式
english.DesktopIcon=Create a Mini DAW desktop shortcut
chinesesimplified.Launch=启动 Mini DAW
english.Launch=Launch Mini DAW
chinesesimplified.OpenPanel=打开 Aggregate Audio 配置程序
english.OpenPanel=Open Aggregate Audio settings
chinesesimplified.Guide=安装与使用说明
english.Guide=Installation guide
chinesesimplified.Busy=Mini DAW 正在运行或安装目录中的文件不可写入。请保存工程并退出 Mini DAW 后重试。
english.Busy=Mini DAW is running or its installed files are not writable. Save your project and exit Mini DAW, then retry.
chinesesimplified.RuntimeError=Microsoft Visual C++ x64 运行库安装失败（错误码 %1）。请查看安装日志后重试。
english.RuntimeError=Microsoft Visual C++ x64 runtime installation failed (code %1). Check the installation log and retry.
chinesesimplified.DriverError=Aggregate Audio ASIO 安装失败（错误码 %1）。请关闭使用该驱动的 DAW、配置程序及诊断工具后重试，或返回取消勾选 ASIO。日志：%2
english.DriverError=Aggregate Audio ASIO installation failed (code %1). Close DAWs using this driver, its settings panel and diagnostics, then retry, or go back and deselect ASIO. Log: %2
chinesesimplified.InstallingRuntime=正在安装 Microsoft Visual C++ x64 运行库…
english.InstallingRuntime=Installing Microsoft Visual C++ x64 runtime...
chinesesimplified.InstallingDriver=正在安装 Aggregate Audio ASIO {#AggregateVersion}…
english.InstallingDriver=Installing Aggregate Audio ASIO {#AggregateVersion}...

[Types]
Name: "full"; Description: "{cm:Full}"
Name: "minionly"; Description: "{cm:MiniOnly}"
Name: "custom"; Description: "{cm:Custom}"; Flags: iscustom

[Components]
Name: "minidaw"; Description: "{cm:MainComponent}"; Types: full minionly custom; Flags: fixed
Name: "aggregate"; Description: "{cm:DriverComponent}"; Types: full; ExtraDiskSpaceRequired: 10485760

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#PayloadDir}\MiniDAW.exe"; DestDir: "{app}"; Components: minidaw; Flags: ignoreversion
Source: "{#PayloadDir}\vc_redist.x64.exe"; Flags: dontcopy
Source: "{#PayloadDir}\AggregateAudio-Setup.exe"; Flags: dontcopy
Source: "INSTALL-zh-CN.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\binaries.sha256"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Mini DAW"; Filename: "{app}\MiniDAW.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:Guide}"; Filename: "{app}\INSTALL-zh-CN.txt"
Name: "{autodesktop}\Mini DAW"; Filename: "{app}\MiniDAW.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{code:AggregateConfigPath}"; Description: "{cm:OpenPanel}"; Check: CanOpenAggregateConfig; Flags: nowait postinstall skipifsilent runasoriginaluser unchecked
Filename: "{app}\MiniDAW.exe"; Description: "{cm:Launch}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var
  AggregateInstalled: Boolean;
  DependencyNeedsRestart: Boolean;

function OpenExistingFile(const FileName: String; Access, Sharing: LongWord;
  Security: NativeUInt; Creation, Attributes: LongWord; Template: NativeUInt): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseFileHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function AggregateConfigPath(Param: String): String;
var
  DriverPath: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM64,
    'SOFTWARE\Classes\CLSID\{D3AA79B2-9D91-4B6F-A2B1-55CE84C2A43D}\InprocServer32', '', DriverPath) and
    FileExists(DriverPath) then
    Result := ExtractFileDir(DriverPath) + '\AggregateAudioConfig.exe';
end;

function CanOpenAggregateConfig: Boolean;
begin
  Result := WizardIsComponentSelected('aggregate') and FileExists(AggregateConfigPath(''));
end;

procedure InitializeWizard;
begin
  { Existing drivers are kept unless the user explicitly selects a reinstall. }
  if not WizardSilent and FileExists(AggregateConfigPath('')) then WizardSelectComponents('minidaw');
end;

function FilesAvailable: Boolean;
var
  Path: String;
  Handle: THandle;
begin
  Result := True;
  Path := ExpandConstant('{app}\MiniDAW.exe');
  if FileExists(Path) then begin
    Handle := OpenExistingFile(Path, $C0000000, 0, 0, 3, $80, 0);
    if Handle = THandle(-1) then begin
      Log('Cannot exclusively open ' + Path);
      Result := False;
    end else CloseFileHandle(Handle);
  end;
end;

function RuntimeInView(RootKey: Integer): Boolean;
var
  Installed, Major, Minor, Build: Cardinal;
  Key: String;
begin
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  Result := RegQueryDWordValue(RootKey, Key, 'Installed', Installed) and
    (Installed = 1) and RegQueryDWordValue(RootKey, Key, 'Major', Major) and
    RegQueryDWordValue(RootKey, Key, 'Minor', Minor) and
    RegQueryDWordValue(RootKey, Key, 'Bld', Build);
  if Result then Result := (Major > 14) or ((Major = 14) and
    ((Minor > 44) or ((Minor = 44) and (Build >= 35112))));
end;

function RuntimePresent: Boolean;
begin
  Result := RuntimeInView(HKLM64) or RuntimeInView(HKLM32);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  DriverLog: String;
begin
  Result := '';
  if not FilesAvailable then begin Result := CustomMessage('Busy'); Exit; end;
  if not RuntimePresent then begin
    WizardForm.PreparingLabel.Caption := CustomMessage('InstallingRuntime');
    ExtractTemporaryFile('vc_redist.x64.exe');
    Code := -1;
    if not Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart', '',
      SW_HIDE, ewWaitUntilTerminated, Code) then begin
      Result := FmtMessage(CustomMessage('RuntimeError'), [IntToStr(Code)]); Exit;
    end;
    Log('Visual C++ runtime exit code: ' + IntToStr(Code));
    if ((Code <> 0) and (Code <> 3010) and (Code <> 1638)) or not RuntimePresent then begin
      Result := FmtMessage(CustomMessage('RuntimeError'), [IntToStr(Code)]); Exit;
    end;
    if Code = 3010 then DependencyNeedsRestart := True;
  end;
  if WizardIsComponentSelected('aggregate') and not AggregateInstalled then begin
    WizardForm.PreparingLabel.Caption := CustomMessage('InstallingDriver');
    ExtractTemporaryFile('AggregateAudio-Setup.exe');
    DriverLog := ExpandConstant('{tmp}\AggregateAudio-install.log');
    Code := -1;
    if not Exec(ExpandConstant('{tmp}\AggregateAudio-Setup.exe'),
      '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /RESTARTEXITCODE=3010 /LOG="' + DriverLog + '"', '',
      SW_HIDE, ewWaitUntilTerminated, Code) then begin
      Result := FmtMessage(CustomMessage('DriverError'), [IntToStr(Code), DriverLog]); Exit;
    end;
    Log('Aggregate Audio installer exit code: ' + IntToStr(Code));
    if (Code <> 0) and (Code <> 3010) then begin
      Result := FmtMessage(CustomMessage('DriverError'), [IntToStr(Code), DriverLog]); Exit;
    end;
    AggregateInstalled := True;
    if Code = 3010 then DependencyNeedsRestart := True;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := DependencyNeedsRestart;
end;

function InitializeUninstall: Boolean;
begin
  Result := FilesAvailable;
  if not Result then SuppressibleMsgBox(CustomMessage('Busy'), mbError, MB_OK, IDOK);
  { Aggregate Audio and the shared runtime retain their own uninstall ownership.
    User presets, plugin caches and audio profiles are outside the app folder. }
end;
