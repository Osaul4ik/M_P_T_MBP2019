@echo off
setlocal

rem %1 = driver base name (no extension), optional. Falls back to the
rem default driver name so this script still works standalone.
set "DRVNAME=%~1"
if "%DRVNAME%"=="" set "DRVNAME=AmtPtpDeviceUsbKm"

set "CERT=%~dp0%DRVNAME%.cer"

rem Import-Certificate into Cert:\LocalMachine\* requires Administrator
rem privileges.
net session >nul 2>&1
if errorlevel 1 (
    echo [ERROR] This script must be run as Administrator.
    pause
    exit /b 1
)

if not exist "%CERT%" (
    echo [ERROR] Certificate "%CERT%" not found.
    pause
    exit /b 1
)

echo Importing certificate into Trusted Root...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CERT%' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"
if errorlevel 1 (
    echo [ERROR] Failed to import the certificate into Trusted Root.
    pause
    exit /b 1
)

echo Importing certificate into Trusted Publishers...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CERT%' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"
if errorlevel 1 (
    echo [ERROR] Failed to import the certificate into Trusted Publishers.
    pause
    exit /b 1
)

echo.
echo Certificate imported successfully.
pause
exit /b 0