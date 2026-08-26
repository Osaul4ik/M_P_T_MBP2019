; ============================================================================
;  WellspringControlCenter.iss
;  Installer for Wellspring Control Center (AmtPtpConfigGui) + optional
;  AmtPtpDeviceUsbKm driver.
;
;  What it does:
;   - At the very start (right after the Welcome page) asks what to install:
;       1) GUI + Driver (default)
;       2) GUI only
;       3) Driver only
;     Only shown when the build actually embeds a driver (see IncludeDriver
;     below) - a GUI-only build behaves exactly like before, no page shown.
;   - Installs the GUI to {autopf}\WellspringControlCenter (Program Files)
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
;   - Driver install (when chosen) runs in-process, right inside setup.exe,
;     NOT via a separate .bat/.vbs. Setup.exe itself already requires admin
;     (PrivilegesRequired=admin below), so it is already elevated by the
;     time it reaches this code - every Exec() call it makes (bcdedit,
;     Import-Certificate, pnputil) inherits that same admin token
;     automatically. There is no second UAC prompt, no self-relaunch, and
;     therefore nothing that can fail the way the old VBS
;     "ShellExecute ... runas" relaunch used to (it would print "Requesting
;     administrator privileges..." and then the console window would just
;     close/crash if the relaunch didn't take).
;   - After a successful driver install, Setup asks to reboot using its own
;     built-in Finished-page mechanism (NeedsRestart) instead of a raw
;     "shutdown /r /t 30" - the user gets the normal Inno "restart now /
;     later" choice.
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

; Optional: folder with the built + signed driver package (.sys/.inf/.cat/
; .cer) - artifacts\driver in BuildAll.yml, passed as
; stage\min\WellspringPTP\Driver from the package-installer job there via
; /DDriverSourceDir=... . Left undefined by BuildGui.yml (GUI-only build has
; no driver to package), in which case the installer behaves exactly as
; before - GUI only, no choice page, "WellspringControlCenter-Setup-*".
;
; When it IS defined, the installer is named "WellspringPTP-Setup-*", shows
; the "what to install" page described above, and the four driver files are
; embedded (Flags: dontcopy - NOT installed under {app}), extracted to
; Setup's own {tmp} folder only for the duration of the install. Whether the
; driver is installed or skipped, Inno Setup deletes everything it
; extracted to {tmp} once Setup exits - nothing driver-related is left
; behind outside of what was actually installed into Windows.
#ifdef DriverSourceDir
  #define IncludeDriver
  #ifndef DriverName
    #define DriverName "AmtPtpDeviceUsbKm"
  #endif
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

#ifdef IncludeDriver
  #define MyOutputBaseFilename "WellspringPTP-Setup-" + MyAppVersion
#else
  #define MyOutputBaseFilename "WellspringControlCenter-Setup-" + MyAppVersion
#endif

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
OutputBaseFilename={#MyOutputBaseFilename}
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
; publish\win-x64 for a local build via build-gui.ps1). Skipped entirely
; when the user picked "Driver only".
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: InstallGui
#ifdef IncludeDriver
; Signed driver package - embedded in setup.exe but NOT copied under {app}.
; dontcopy means these are only extracted (to {tmp}) on demand, via
; ExtractTemporaryFile in [Code]; see CurStepChanged below. DestDir is
; required syntax here but unused for dontcopy entries. Check: InstallDriver
; means these are simply never extracted when the user picked "GUI only".
Source: "{#DriverSourceDir}\{#DriverName}.sys"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.inf"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.cat"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
Source: "{#DriverSourceDir}\{#DriverName}.cer"; DestDir: "{tmp}"; Flags: dontcopy; Check: InstallDriver
#endif

[Dirs]
; Shared settings/profiles folder for all users. The GUI normally runs
; without elevation, so grant BUILTIN\Users modify rights upfront -
; otherwise a non-admin user couldn't write there. uninsneveruninstall:
; deletion is handled by hand in [Code] (asked for and only on consent),
; not silently by Inno's own automation. Only needed when the GUI is
; actually being installed.
Name: "{commonappdata}\WellspringPTP"; Permissions: users-modify; Flags: uninsneveruninstall; Check: InstallGui

[Icons]
; Shortcut only, NO subfolder in Start Menu, for all users.
Name: "{commonprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Check: InstallGui

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent; Check: InstallGui

[Code]

const
  ProcessName = 'Wellspring Control Center.exe';
  SettingsDirName = 'WellspringPTP';
  RunKeyPath = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValueName = 'WellspringPTP';

var
  #ifdef IncludeDriver
  InstallChoicePage: TInputOptionWizardPage;
  #endif
  RebootNeeded: Boolean;

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

procedure WarnIfDotNet8Missing;
begin
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

#ifdef IncludeDriver
// "What to install" page, shown right after the Welcome page - only
// present in builds that actually embed a driver. Exclusive radio-button
// style list (True = exclusive): 0 = GUI + Driver, 1 = GUI only,
// 2 = Driver only.
procedure InitializeWizard();
begin
  InstallChoicePage := CreateInputOptionPage(wpWelcome,
    'Select Installation Type',
    'What would you like to install?',
    'Select one option, then click Next to continue.',
    True, False);
  InstallChoicePage.Add('GUI + Driver (recommended)');
  InstallChoicePage.Add('GUI only');
  InstallChoicePage.Add('Driver only');
  InstallChoicePage.SelectedValueIndex := 0;
end;

function InstallGui(): Boolean;
begin
  Result := InstallChoicePage.SelectedValueIndex <> 2;
end;

function InstallDriver(): Boolean;
begin
  Result := InstallChoicePage.SelectedValueIndex <> 1;
end;

// Runs the driver install steps directly inside this (already-elevated)
// setup.exe process - no separate script, no self-relaunch, no VBS.
// bcdedit / Import-Certificate / pnputil all just need SOME admin process
// to run them; since PrivilegesRequired=admin above already guarantees
// setup.exe itself is elevated, every Exec() call here inherits that same
// token automatically.
procedure InstallDriverNow;
var
  ResultCode: Integer;
  Inf, Cer: String;
  Ok: Boolean;
begin
  Inf := ExpandConstant('{tmp}\{#DriverName}.inf');
  Cer := ExpandConstant('{tmp}\{#DriverName}.cer');
  Ok := True;

  WizardForm.StatusLabel.Caption := 'Enabling Windows Test Mode...';
  if (not Exec(ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning on', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    MsgBox('Could not enable Test Mode (bcdedit).' + #13#10 +
      'If Secure Boot is on in the BIOS/UEFI, it must be disabled first.' + #13#10 +
      'Driver installation was NOT completed.', mbError, MB_OK);
    Ok := False;
  end;

  if Ok and (not FileExists(Cer)) then
  begin
    MsgBox('Certificate not found: ' + Cer, mbError, MB_OK);
    Ok := False;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Importing certificate (Trusted Root)...';
    if (not Exec('powershell.exe',
         '-NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath ''' +
         Cer + ''' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox('Failed to import the certificate into Trusted Root.', mbError, MB_OK);
      Ok := False;
    end;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Importing certificate (Trusted Publishers)...';
    if (not Exec('powershell.exe',
         '-NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath ''' +
         Cer + ''' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox('Failed to import the certificate into Trusted Publishers.', mbError, MB_OK);
      Ok := False;
    end;
  end;

  if Ok and (not FileExists(Inf)) then
  begin
    MsgBox('Driver .inf not found: ' + Inf, mbError, MB_OK);
    Ok := False;
  end;

  if Ok then
  begin
    WizardForm.StatusLabel.Caption := 'Installing driver (pnputil)...';
    if (not Exec(ExpandConstant('{sys}\pnputil.exe'), '/add-driver "' + Inf + '" /install',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      MsgBox('pnputil reported an error installing the driver.', mbError, MB_OK);
      Ok := False;
    end;
  end;

  if Ok then
  begin
    RebootNeeded := True;
    MsgBox('The driver was installed successfully.' + #13#10 +
      'A reboot is required to finish - you will be offered one on the next page.',
      mbInformation, MB_OK);
  end;
end;

// Driver files were embedded with Flags: dontcopy, so they don't exist on
// disk until explicitly extracted. Do that right after the app itself is
// installed, then (if the user chose to install the driver) ask for
// confirmation and run the steps in-process. Inno Setup deletes everything
// it extracted to {tmp} when Setup exits, whether the driver install
// succeeds, is declined, or fails - no manual cleanup needed here.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and InstallDriver() then
  begin
    ExtractTemporaryFile('{#DriverName}.sys');
    ExtractTemporaryFile('{#DriverName}.inf');
    ExtractTemporaryFile('{#DriverName}.cat');
    ExtractTemporaryFile('{#DriverName}.cer');

    if MsgBox(
      'Install the Wellspring PTP driver now?' + #13#10#13#10 +
      'This will:' + #13#10 +
      '  1. Enable Windows Test Mode (testsigning) so the driver''s test' + #13#10 +
      '     certificate is trusted.' + #13#10 +
      '  2. Import that certificate into Trusted Root and Trusted Publishers.' + #13#10 +
      '  3. Install the {#DriverName} driver (pnputil).' + #13#10 +
      '  4. Require a reboot afterwards to finish.' + #13#10#13#10 +
      'Save any open work before continuing.',
      mbConfirmation, MB_YESNO) = IDYES then
      InstallDriverNow;
  end;
end;

function NeedsRestart(): Boolean;
begin
  Result := RebootNeeded;
end;

#else
function InstallGui(): Boolean;
begin
  Result := True;
end;

function InstallDriver(): Boolean;
begin
  Result := False;
end;
#endif

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
