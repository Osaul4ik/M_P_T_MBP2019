; ============================================================================
;  WellspringControlCenter.iss
;  Installer for Wellspring Control Center (AmtPtpConfigGui).
;
;  What it does:
;   - Installs to {autopf}\WellspringControlCenter (Program Files)
;   - Registers an entry in "Programs and Features" (Inno Setup does this
;     automatically: HKLM\...\Uninstall\{AppId}_is1)
;   - Before copying files (fresh install OR upgrade), force-closes
;     "Wellspring Control Center.exe" if it's running, so the file can be
;     replaced
;   - Start Menu shortcut for ALL users, with NO subfolder:
;     {commonprograms}\Wellspring Control Center.lnk
;   - Creates {commonappdata}\WellspringPTP (C:\ProgramData\WellspringPTP)
;     with write permissions for BUILTIN\Users - the GUI stores
;     settings.json/profiles.json there, SHARED across all users on the
;     machine
;   - On uninstall: closes the process, removes the shortcut
;     (automatically), and asks whether to delete settings/profiles
;     (%ProgramData%\WellspringPTP) and the autostart key
;     HKCU\...\Run\WellspringPTP for the current user
;
;  Lives at src/AmtPtpConfigGui/installer/WellspringControlCenter.iss.
;
;  Build: Inno Setup 6.x (innosetup.org), command (from repo root):
;     ISCC.exe src\AmtPtpConfigGui\installer\WellspringControlCenter.iss /DMyAppVersion=1.0.0
;
;  Before compiling, run build-gui.ps1 (same folder) - the installer takes
;  files from publish\win-x64 relative to this same installer\ folder
;  ([Files] below; overridden via /DSourceDir from CI).
; ============================================================================

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

; Folder with the published GUI. From CI (BuildGui.yml) this is
; stage\min\WellspringPTP\Gui - the Min artifact (framework-dependent
; single file). For a local build via build-gui.ps1 - publish\win-x64.
#ifndef SourceDir
  #define SourceDir "publish\win-x64"
#endif

#define MyAppName "Wellspring Control Center"
#define MyAppExeName "Wellspring Control Center.exe"
#define MyAppPublisher "WellspringPTP"
#define MyAppURL "https://github.com/Osaul4ik/wellspring-ptp"
; Fixed GUID - do NOT change it between releases, or an upgrade will stop
; being recognized as an update of the previous install.
; "{{" at the start - in Inno Setup a single "{" is always read as the
; start of a constant reference (like {app}), so to get a literal "{" in
; the final AppId value the opening brace has to be doubled; the closing
; "}" does not need escaping - it isn't a special character.
#define MyAppId "{{A6C1E4B0-6C1B-4B7E-9C0A-6E9E7C6E5D01}"

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
; Published GUI (Min - framework-dependent single file from CI, or
; publish\win-x64 for a local build via build-gui.ps1).
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
; Shared settings/profiles folder for all users. The GUI normally runs
; without elevation, so grant BUILTIN\Users modify rights upfront -
; otherwise a non-admin user couldn't write there. uninsneveruninstall:
; deletion is handled by hand in [Code] (asked for and only on consent),
; not silently by Inno's own automation.
Name: "{commonappdata}\WellspringPTP"; Permissions: users-modify; Flags: uninsneveruninstall

[Icons]
; Shortcut only, NO subfolder in Start Menu, for all users.
Name: "{commonprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]

const
  ProcessName = 'Wellspring Control Center.exe';
  SettingsDirName = 'WellspringPTP';
  RunKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValueName = 'WellspringPTP';

// Min build - framework-dependent (no bundled CLR), so .NET 8 Desktop
// Runtime must be installed on the machine. Check the shared-runtime
// directory; if not found, only warn - don't block the install.
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
    MsgBox('.NET 8 Desktop Runtime was not found.' + #13#10 +
      'This build of Wellspring Control Center requires it to run.' + #13#10 +
      'Install it from https://dotnet.microsoft.com/download/dotnet/8.0 ' +
      '("Desktop Runtime" section), then run the installer again, or ' +
      'install the runtime later - setup will continue.',
      mbInformation, MB_OK);
  end;
end;

// Force-closes the GUI if it's running (needed both for a clean install
// over an older version and for uninstall).
procedure KillRunningApp;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM "' + ProcessName + '" /T',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Small pause so the OS has time to release the exe's file handles.
  Sleep(400);
end;

// Runs before files are copied - both on a first install and on an
// upgrade over an existing one (Inno determines this itself via AppId).
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
    // Shared folder for all users - unlike the former {userappdata},
    // deleting this affects ALL accounts on the machine, not just the
    // one that ran the uninstall.
    SettingsDir := ExpandConstant('{commonappdata}\') + SettingsDirName;

    DeleteSettings := (MsgBox(
      'Delete Wellspring Control Center settings and profiles?' + #13#10 +
      SettingsDir + #13#10 +
      '(shared across all users on this machine)',
      mbConfirmation, MB_YESNO) = IDYES);

    if DeleteSettings then
    begin
      DelTree(SettingsDir, True, True, True);
      // The autostart key is a per-user Windows mechanism (HKCU), so
      // it's only removed for the account that ran the uninstall; if
      // other users enabled autostart under their own accounts, their
      // entries will remain.
      RegDeleteValue(HKEY_CURRENT_USER, RunKeyPath, RunValueName);
    end;
  end;
end;
