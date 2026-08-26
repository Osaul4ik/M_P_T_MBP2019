#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

#ifndef SourceDir
  #define SourceDir "publish\win-x64"
#endif

#define MyAppName "Wellspring Control Center"
#define MyAppExeName "Wellspring Control Center.exe"
#define MyAppPublisher "WellspringPTP"
#define MyAppURL "https://github.com/Osaul4ik/wellspring-ptp"
#define MyAppId "{A6C1E4B0-6C1B-4B7E-9C0A-6E9E7C6E5D01}"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\WellspringControlCenter
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableWelcomePage=no
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
OutputBaseFilename=WellspringControlCenter-Setup-{#MyAppVersion}
OutputDir=output
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{commonappdata}\WellspringPTP"; Permissions: users-modify; Flags: uninsneveruninstall

[Icons]
; Single shortcut, without a Start Menu subfolder, for all users.
Name: "{commonprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]

const
  ProcessName = 'Wellspring Control Center.exe';
  SettingsDirName = 'WellspringPTP';
  RunKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValueName = 'WellspringPTP';

// Min build - framework-dependent (without bundled CLR), so .NET 8
// Desktop Runtime must be installed on the machine. We check the
// shared runtime directory; if it is not found, we only warn and
// do not block the installation.
function IsDotNet8DesktopRuntimeInstalled(): Boolean;
var
  BaseDir: String;
  FindRec: TFindRec;
  Found: Boolean;
begin
  Found := False;
  BaseDir := ExpandConstant('{commonpf}\dotnet\shared\Microsoft.WindowsDesktop.App');
  if DirExists(BaseDir) then
  begin
    if FindFirst(BaseDir + '\8.*', FindRec) then
    begin
      Found := True;
      FindClose(FindRec);
    end;
  end;
  Result := Found;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsDotNet8DesktopRuntimeInstalled() then
  begin
    MsgBox('The .NET 8 Desktop Runtime was not found.' + #13#10 +
      'This build of Wellspring Control Center requires it to run.' + #13#10 +
      'Please install it from https://dotnet.microsoft.com/download/dotnet/8.0 ' +
      '(under "Desktop Runtime"), then run the installer again, ' +
      'or install the runtime later - the installation will continue.',
      mbInformation, MB_OK);
  end;
end;

// Forcefully terminates the GUI if it is running (required both for a clean
// installation over an older version and for uninstallation).
procedure KillRunningApp;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM "' + ProcessName + '" /T',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Brief pause to allow the OS to release the executable's file handles.
  Sleep(400);
end;

// Called before copying files - both for a first installation and for an
// update over an existing installation (Inno determines this by AppId).
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillRunningApp;
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  KillRunningApp;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  SettingsDir: String;
  DeleteSettings: Boolean;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Shared by all users - unlike the former {userappdata}, deletion here
    // affects ALL accounts on the machine, not only the account that
    // started the uninstallation.
    SettingsDir := ExpandConstant('{commonappdata}\') + SettingsDirName;

    DeleteSettings := (MsgBox(
      'Delete Wellspring Control Center settings and profiles?' + #13#10 +
      SettingsDir + #13#10 +
      '(shared by all users on this machine)',
      mbConfirmation, MB_YESNO) = IDYES);

    if DeleteSettings then
    begin
      DelTree(SettingsDir, True, True, True);
      RegDeleteValue(HKEY_CURRENT_USER, RunKeyPath, RunValueName);
    end;
  end;
end;
