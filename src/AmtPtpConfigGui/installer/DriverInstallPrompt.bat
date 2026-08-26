@echo off
setlocal
title Wellspring PTP - Driver Installation

rem %1 = driver base name (no extension), passed in from the .iss [Run] line.
rem This script and the driver files (.sys/.inf/.cat/.cer) are all extracted
rem by Setup into the same temp folder, so %~dp0 (this script's own folder)
rem is where they live - no other path needs to be known.
set "DRVNAME=%~1"
if "%DRVNAME%"=="" set "DRVNAME=AmtPtpDeviceUsbKm"

set "HERE=%~dp0"
set "INF=%HERE%%DRVNAME%.inf"
set "CER=%HERE%%DRVNAME%.cer"

rem Must run elevated: bcdedit, pnputil and Import-Certificate into
rem Cert:\LocalMachine\* all require Administrator privileges.
rem If not elevated yet, relaunch this same script with a UAC prompt,
rem preserving the driver-name argument and working folder, then exit
rem this non-elevated instance.
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator privileges...
    set "ELEVATE_VBS=%TEMP%\wsp_elevate_%RANDOM%.vbs"
    > "%ELEVATE_VBS%" echo On Error Resume Next
    >> "%ELEVATE_VBS%" echo Set UAC = CreateObject("Shell.Application")
    >> "%ELEVATE_VBS%" echo UAC.ShellExecute "%~f0", "%DRVNAME%", "%HERE%", "runas", 1
    cscript //nologo "%ELEVATE_VBS%"
    del "%ELEVATE_VBS%" >nul 2>&1
    goto END
)

:ASK
cls
echo =====================================================
echo   Wellspring PTP - Driver Installation
echo =====================================================
echo.
echo This will:
echo   1. Enable Windows Test Mode (testsigning) so the driver's
echo      test certificate is trusted.
echo   2. Import that certificate into Trusted Root and Trusted
echo      Publishers.
echo   3. Install the %DRVNAME% driver (pnputil).
echo   4. Reboot this computer - REQUIRED for Test Mode to take
echo      effect and for the driver to load.
echo.
echo Save any open work before continuing.
echo.

choice /c YN /n /m "Install the driver and reboot now? [Y]es / [N]o: "

if errorlevel 2 goto SKIP
if errorlevel 1 goto INSTALL

goto ASK

:INSTALL
echo.
echo [1/4] Enabling Test Mode (bcdedit /set testsigning on) ...
bcdedit /set testsigning on
if errorlevel 1 (
    echo.
    echo [ERROR] Could not enable Test Mode. If Secure Boot is on in the
    echo         BIOS/UEFI, it must be disabled first. Driver installation
    echo         was NOT completed.
    echo.
    pause
    goto END
)

if not exist "%CER%" (
    echo.
    echo [ERROR] Certificate not found: %CER%
    echo.
    pause
    goto END
)

echo.
echo [2/4] Importing certificate into Trusted Root ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"
if errorlevel 1 (
    echo.
    echo [ERROR] Failed to import the certificate into Trusted Root.
    echo.
    pause
    goto END
)

echo [2/4] Importing certificate into Trusted Publishers ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CER%' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"
if errorlevel 1 (
    echo.
    echo [ERROR] Failed to import the certificate into Trusted Publishers.
    echo.
    pause
    goto END
)

if not exist "%INF%" (
    echo.
    echo [ERROR] Driver .inf not found: %INF%
    echo.
    pause
    goto END
)

echo.
echo [3/4] Installing driver (pnputil) ...
pnputil /add-driver "%INF%" /install
if errorlevel 1 (
    echo.
    echo [ERROR] pnputil reported an error installing the driver.
    echo.
    pause
    goto END
)

echo.
echo [4/4] Rebooting in 30 seconds to finish driver installation.
echo       Run "shutdown /a" in another window to cancel the reboot.
shutdown /r /t 30 /c "Wellspring PTP: reboot required to finish driver installation."
goto END

:SKIP
echo.
echo Skipping driver installation. You can rerun the installer later
echo if you change your mind.
echo.
pause >nul
goto END

:END
endlocal
exit /b 0