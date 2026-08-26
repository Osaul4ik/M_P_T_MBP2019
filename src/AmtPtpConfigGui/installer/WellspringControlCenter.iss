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
;   - If the user's choice on the page above includes the driver ("GUI +
;     Driver" or "Driver only"), a warning is shown right after clicking
;     Next on that page explaining that a restart will be required and
;     that Setup will close and reboot the machine automatically once the
;     driver install finishes - no "restart now / later" choice is offered
;     later on, it just happens.
;   - After a successful driver install, Setup no longer waits for the
;     Finished page: it schedules the restart itself (shutdown /r) and
;     closes its own window as soon as the installation step completes.
;   - On uninstall (only when the build embeds a driver - see IncludeDriver):
;     asks what to remove - "Driver + GUI", "GUI only", or "Driver only".
;       * Driver + GUI: driver is removed (with the Test Mode / rescan
;         steps below), then Inno's normal full uninstall proceeds and
;         removes everything, including the uninstaller itself.
;       * GUI only: only the GUI's files/shortcut are removed; the driver
;         is left untouched. The Programs-and-Features entry AND
;         unins000.exe/.dat are deliberately kept, since the driver still
;         needs a way to be uninstalled later.
;       * Driver only: only the driver is removed; the GUI is left
;         untouched, and likewise the uninstaller entry is kept.
;     In short: the uninstaller only fully removes/deletes itself once
;     BOTH the GUI and the driver have been removed - if either one still
;     remains, "Wellspring Control Center" stays listed in Programs and
;     Features so it can be run again later.
;     Whenever the driver itself is being removed (Driver + GUI, or Driver
;     only), the user is also asked whether to disable Windows Test Mode
;     (testsigning); if they say yes, it's turned off. After the driver
;     package is removed, a device rescan (pnputil /scan-devices) is run
;     so the touchpad gets rebound to a working driver right away instead
;     of being left dead until next boot.
;     A GUI-only build (no IncludeDriver) keeps the old, simpler behavior:
;     no choice page, plain full removal.
;   - On any uninstall path that removes the GUI, the user is also asked
;     whether to delete settings/profiles (%ProgramData%\WellspringPTP)
;     and the autostart key HKCU\...\Run\WellspringPTP for the current
;     user.
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
  // 0 = Driver + GUI, 1 = GUI only, 2 = Driver only - set by
  // AskUninstallChoice() in InitializeUninstall, consumed by
  // CurUninstallStepChanged.
  UninstallChoice: Integer;
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

// Warns about the upcoming reboot the moment the user commits to a
// driver-inclusive choice (index 0 = GUI + Driver, index 2 = Driver only)
// on the selection page, before any files are copied - not just later,
// buried in the driver-install confirmation dialog.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = InstallChoicePage.ID) and
     (InstallChoicePage.SelectedValueIndex <> 1) then
  begin
    MsgBox(
      'Installing the driver requires a restart to finish.' + #13#10#13#10 +
      'Once the driver has been installed, Setup will close automatically ' +
      'and your computer will restart - no further confirmation will be ' +
      'asked at that point.' + #13#10#13#10 +
      'Please save any open work before continuing.',
      mbInformation, MB_OK);
  end;
end;

function InstallGui(): Boolean;
begin
  Result := InstallChoicePage.SelectedValueIndex <> 2;
end;

function InstallDriver(): Boolean;
begin
  Result := InstallChoicePage.SelectedValueIndex <> 1;
end;

// Finds every OLD published Driver Store package that originated from
// our own AmtPtpDeviceUsbKm.inf (regardless of what "oemNN.inf" number
// Windows assigned it) and removes it - both from the Driver Store and
// from any device currently bound to it - before the new version is
// installed. Without this, repeated installs just keep piling up
// oemNN.inf copies of the same driver, and Windows is sometimes left
// pointed at a stale one.
//
// We deliberately do NOT parse `pnputil /enum-drivers` text output - it
// is localized (headers like "Original Name:" change with Windows'
// display language), so matching against it is unreliable on non-English
// systems. Get-WindowsDriver (DISM PowerShell module) exposes the same
// data through the OriginalFileName .NET property, which is always in
// English no matter the UI language.
procedure RemoveOldDriverVersions;
var
  ResultCode: Integer;
  ListFile, PsCmd, Name: String;
  OldDrivers: TArrayOfString;
  I: Integer;
begin
  WizardForm.StatusLabel.Caption := 'Checking for previous driver versions...';
  ListFile := ExpandConstant('{tmp}\wellspring_old_drivers.txt');
  if FileExists(ListFile) then
    DeleteFile(ListFile);

  PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command ' +
    '"Get-WindowsDriver -Online -All | ' +
    'Where-Object { $_.OriginalFileName -like ''*{#DriverName}.inf'' } | ' +
    'Select-Object -ExpandProperty Driver | ' +
    'Set-Content -Path ''' + ListFile + ''' -Encoding ascii"';

  // Best-effort: if Get-WindowsDriver isn't available/fails for any
  // reason, just skip cleanup rather than blocking the install - the new
  // driver still gets added either way.
  if (not Exec('powershell.exe', PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode))
     or (ResultCode <> 0) or (not FileExists(ListFile)) then
    Exit;

  if not LoadStringsFromFile(ListFile, OldDrivers) then
    Exit;

  for I := 0 to GetArrayLength(OldDrivers) - 1 do
  begin
    Name := Trim(OldDrivers[I]);
    if Name = '' then
      Continue;
    WizardForm.StatusLabel.Caption := 'Removing previous driver (' + Name + ')...';
    // /uninstall also detaches it from any device still using it;
    // /force suppresses the "in use" confirmation prompt. Ignore the
    // result - if one old package can't be removed, move on and let the
    // new /add-driver /install below take over anyway.
    Exec(ExpandConstant('{sys}\pnputil.exe'), '/delete-driver ' + Name + ' /uninstall /force',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
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

  RemoveOldDriverVersions;

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
    // pnputil legitimately returns 3010 (ERROR_SUCCESS_REBOOT_REQUIRED)
    // when the driver installed fine but a reboot is needed to finish
    // binding it to the device - that is NOT a failure. Only treat the
    // Exec() call itself failing, or a genuinely unknown/error code, as
    // a real problem.
    if not Exec(ExpandConstant('{sys}\pnputil.exe'), '/add-driver "' + Inf + '" /install',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    begin
      MsgBox('Could not launch pnputil.exe.', mbError, MB_OK);
      Ok := False;
    end
    else if (ResultCode <> 0) and (ResultCode <> 3010) then
    begin
      MsgBox('pnputil reported an error installing the driver (code ' +
        IntToStr(ResultCode) + ').', mbError, MB_OK);
      Ok := False;
    end
    else if ResultCode = 3010 then
      RebootNeeded := True;
  end;

  if Ok then
  begin
    // Best-effort nudge so PnP re-evaluates the device against the
    // freshly-installed package right away; harmless either way since a
    // reboot is required regardless (result intentionally ignored).
    Exec(ExpandConstant('{sys}\pnputil.exe'), '/scan-devices', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // No confirmation dialog here on purpose - the user was already
    // warned about the restart on the selection page. Setup closes and
    // the machine reboots automatically once CurStepChanged reaches
    // ssDone below.
    WizardForm.StatusLabel.Caption :=
      'Driver installed. Setup will close and the computer will restart shortly...';
    RebootNeeded := True;
  end;
end;

// Driver files were embedded with Flags: dontcopy, so they don't exist on
// disk until explicitly extracted. Do that right after the app itself is
// installed, then (if the user chose to install the driver) ask for
// confirmation and run the steps in-process. Inno Setup deletes everything
// it extracted to {tmp} when Setup exits, whether the driver install
// succeeds, is declined, or fails - no manual cleanup needed here.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
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
      '  4. Automatically close Setup and restart the computer to finish.' + #13#10#13#10 +
      'Save any open work before continuing.',
      mbConfirmation, MB_YESNO) = IDYES then
      InstallDriverNow;
  end;

  // Installation is fully done at this point (files copied, driver step
  // already ran above) - safe to schedule the restart and close our own
  // window without Inno treating it as an aborted/incomplete Setup and
  // asking "Setup is not complete, exit anyway?". This replaces the old
  // Finished-page "restart now / later" choice entirely: it just happens.
  if (CurStep = ssDone) and RebootNeeded then
  begin
    Exec(ExpandConstant('{sys}\shutdown.exe'),
      '/r /t 15 /c "Wellspring PTP: reboot required to finish driver installation." /f',
      '', SW_HIDE, ewNoWait, ResultCode);
    WizardForm.Close;
  end;
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

// Shared by every uninstall path that removes the GUI (both the normal
// "Driver + GUI" flow and the manual "GUI only" flow below). Asks once,
// deletes the shared ProgramData settings folder and the per-user
// autostart key if the user agrees.
procedure MaybeDeleteSettings;
var
  SettingsDir: String;
begin
  // Shared folder for all users - unlike the former {userappdata},
  // deleting this affects ALL accounts on the machine, not just the one
  // that ran the uninstall.
  SettingsDir := ExpandConstant('{commonappdata}\') + SettingsDirName;

  if MsgBox(
    'Delete Wellspring Control Center settings and profiles?' + #13#10 +
    SettingsDir + #13#10 +
    '(shared across all users on this machine)',
    mbConfirmation, MB_YESNO) = IDYES then
  begin
    DelTree(SettingsDir, True, True, True);
    // The autostart key is a per-user Windows mechanism (HKCU), so it's
    // only removed for the account that ran the uninstall; if other
    // users enabled autostart under their own accounts, their entries
    // will remain.
    RegDeleteValue(HKEY_CURRENT_USER, RunKeyPath, RunValueName);
  end;
end;

#ifdef IncludeDriver

// Small custom dialog (there is no wizard during uninstall, so this is
// built by hand with CreateCustomForm) letting the user pick exactly
// what to remove. Returns 0 = Driver + GUI, 1 = GUI only, 2 = Driver
// only, or -1 if the user cancelled.
function AskUninstallChoice(): Integer;
var
  UninstallForm: TSetupForm;
  Lbl: TNewStaticText;
  RadioBoth, RadioGuiOnly, RadioDriverOnly: TNewRadioButton;
  OKButton, CancelButton: TNewButton;
begin
  Result := -1;
  // As of Inno Setup 6.6.0, CreateCustomForm takes the size upfront
  // (ClientWidth/ClientHeight became read-only properties afterward) -
  // False/False here means the form doesn't grow with WizardSizePercent.
  UninstallForm := CreateCustomForm(ScaleX(420), ScaleY(210), False, False);
  try
    UninstallForm.Caption := 'Uninstall Wellspring PTP';
    UninstallForm.Position := poScreenCenter;

    Lbl := TNewStaticText.Create(UninstallForm);
    Lbl.Parent := UninstallForm;
    Lbl.Left := ScaleX(16);
    Lbl.Top := ScaleY(16);
    Lbl.Width := UninstallForm.ClientWidth - ScaleX(32);
    Lbl.AutoSize := False;
    Lbl.WordWrap := True;
    Lbl.Caption := 'What would you like to remove?';

    RadioBoth := TNewRadioButton.Create(UninstallForm);
    RadioBoth.Parent := UninstallForm;
    RadioBoth.Left := ScaleX(16);
    RadioBoth.Top := ScaleY(48);
    RadioBoth.Width := UninstallForm.ClientWidth - ScaleX(32);
    RadioBoth.Caption := 'Driver and GUI (remove everything)';
    RadioBoth.Checked := True;

    RadioGuiOnly := TNewRadioButton.Create(UninstallForm);
    RadioGuiOnly.Parent := UninstallForm;
    RadioGuiOnly.Left := ScaleX(16);
    RadioGuiOnly.Top := ScaleY(76);
    RadioGuiOnly.Width := UninstallForm.ClientWidth - ScaleX(32);
    RadioGuiOnly.Caption := 'GUI only (keep the driver installed)';

    RadioDriverOnly := TNewRadioButton.Create(UninstallForm);
    RadioDriverOnly.Parent := UninstallForm;
    RadioDriverOnly.Left := ScaleX(16);
    RadioDriverOnly.Top := ScaleY(104);
    RadioDriverOnly.Width := UninstallForm.ClientWidth - ScaleX(32);
    RadioDriverOnly.Caption := 'Driver only (keep the GUI installed)';

    Lbl := TNewStaticText.Create(UninstallForm);
    Lbl.Parent := UninstallForm;
    Lbl.Left := ScaleX(16);
    Lbl.Top := ScaleY(136);
    Lbl.Width := UninstallForm.ClientWidth - ScaleX(32);
    Lbl.AutoSize := False;
    Lbl.WordWrap := True;
    Lbl.Caption :=
      'As long as either the driver or the GUI remains installed, this ' +
      'uninstaller stays available in Programs and Features.';

    OKButton := TNewButton.Create(UninstallForm);
    OKButton.Parent := UninstallForm;
    OKButton.Width := ScaleX(75);
    OKButton.Height := ScaleY(23);
    OKButton.Left := UninstallForm.ClientWidth - ScaleX(16) - OKButton.Width - ScaleX(85);
    OKButton.Top := UninstallForm.ClientHeight - ScaleY(16) - OKButton.Height;
    OKButton.Caption := 'OK';
    OKButton.ModalResult := mrOk;
    OKButton.Default := True;

    CancelButton := TNewButton.Create(UninstallForm);
    CancelButton.Parent := UninstallForm;
    CancelButton.Width := ScaleX(75);
    CancelButton.Height := ScaleY(23);
    CancelButton.Left := UninstallForm.ClientWidth - ScaleX(16) - CancelButton.Width;
    CancelButton.Top := UninstallForm.ClientHeight - ScaleY(16) - CancelButton.Height;
    CancelButton.Caption := 'Cancel';
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;

    if UninstallForm.ShowModal() = mrOk then
    begin
      if RadioBoth.Checked then
        Result := 0
      else if RadioGuiOnly.Checked then
        Result := 1
      else if RadioDriverOnly.Checked then
        Result := 2;
    end;
  finally
    UninstallForm.Free;
  end;
end;

// Removes every installed package originating from our own INF
// (regardless of what "oemNN.inf" number Windows assigned it - same
// Get-WindowsDriver matching approach used on install, see
// RemoveOldDriverVersions above), then asks about Test Mode and rescans
// devices so the touchpad doesn't sit dead until next boot.
procedure RemoveDriverNow;
var
  ResultCode: Integer;
  ListFile, PsCmd, Name: String;
  OldDrivers: TArrayOfString;
  I: Integer;
begin
  ListFile := ExpandConstant('{tmp}\wellspring_uninstall_drivers.txt');
  if FileExists(ListFile) then
    DeleteFile(ListFile);

  PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command ' +
    '"Get-WindowsDriver -Online -All | ' +
    'Where-Object { $_.OriginalFileName -like ''*{#DriverName}.inf'' } | ' +
    'Select-Object -ExpandProperty Driver | ' +
    'Set-Content -Path ''' + ListFile + ''' -Encoding ascii"';

  // Best-effort: if this fails for any reason, fall through to the
  // rescan anyway rather than leaving the touchpad in limbo.
  if Exec('powershell.exe', PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
     and (ResultCode = 0) and FileExists(ListFile)
     and LoadStringsFromFile(ListFile, OldDrivers) then
  begin
    for I := 0 to GetArrayLength(OldDrivers) - 1 do
    begin
      Name := Trim(OldDrivers[I]);
      if Name <> '' then
        // /uninstall also detaches it from any device still using it;
        // /force suppresses the "in use" confirmation prompt.
        Exec(ExpandConstant('{sys}\pnputil.exe'), '/delete-driver ' + Name + ' /uninstall /force',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    end;
  end;

  // Relevant whenever the driver itself is being removed - ask whether
  // to also turn Test Mode back off.
  if MsgBox(
    'Disable Windows Test Mode (testsigning)?' + #13#10 +
    'Only say No if some other test-signed driver on this machine still ' +
    'needs it.',
    mbConfirmation, MB_YESNO) = IDYES then
    Exec(ExpandConstant('{sys}\bcdedit.exe'), '/set testsigning off', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // Without this, the touchpad is left with no bound driver at all until
  // the next reboot - rescanning lets Windows re-bind it (to its default
  // HID driver, or back to ours if something went wrong above) right away.
  Exec(ExpandConstant('{sys}\pnputil.exe'), '/scan-devices', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// "GUI only" path: deletes every file directly under {app} except
// Inno's own unins*.exe/.dat (so the uninstaller keeps working) and any
// subfolders in full, plus the Start Menu shortcut and the autostart
// key. Deliberately does NOT touch the "Programs and Features" registry
// entry - that's what keeps the uninstaller listed and runnable.
procedure RemoveGuiFilesKeepUninstaller;
var
  AppDir, Shortcut, EntryPath: String;
  FindRec: TFindRec;
begin
  AppDir := ExpandConstant('{app}');
  Shortcut := ExpandConstant('{commonprograms}\{#MyAppName}.lnk');
  if FileExists(Shortcut) then
    DeleteFile(Shortcut);

  if FindFirst(AppDir + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          EntryPath := AppDir + '\' + FindRec.Name;
          if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DelTree(EntryPath, True, True, True)
          else if Pos('unins', Lowercase(FindRec.Name)) <> 1 then
            DeleteFile(EntryPath);
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

#endif

function InitializeUninstall(): Boolean;
#ifdef IncludeDriver
var
  Choice: Integer;
#endif
begin
  KillRunningApp;
  Result := True;

  #ifdef IncludeDriver
  Choice := AskUninstallChoice();
  if Choice = -1 then
  begin
    Result := False;
    Exit;
  end;
  UninstallChoice := Choice;

  if Choice = 1 then
  begin
    // GUI only: handled entirely here, since returning False below skips
    // Inno's normal file/registry removal (and its self-delete) - that's
    // exactly what keeps the driver-uninstall option available later.
    RemoveGuiFilesKeepUninstaller;
    MaybeDeleteSettings;
    MsgBox(
      'Wellspring Control Center has been removed.' + #13#10 +
      'The driver is still installed - run this uninstaller again if you ' +
      'want to remove it too.',
      mbInformation, MB_OK);
    Result := False;
    Exit;
  end
  else if Choice = 2 then
  begin
    // Driver only: same idea, but nothing under {app} is touched.
    RemoveDriverNow;
    MsgBox(
      'The driver has been removed.' + #13#10 +
      'Wellspring Control Center is still installed - run this ' +
      'uninstaller again if you want to remove it too.',
      mbInformation, MB_OK);
    Result := False;
    Exit;
  end;
  // Choice = 0 (Driver + GUI): fall through with Result = True and let
  // Inno's normal uninstall proceed - CurUninstallStepChanged below
  // removes the driver first, then Inno removes the GUI's files,
  // shortcut, registry entry, and finally itself.
  #endif
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    #ifdef IncludeDriver
    if UninstallChoice = 0 then
      RemoveDriverNow;
    #endif
    MaybeDeleteSettings;
  end;
end;
