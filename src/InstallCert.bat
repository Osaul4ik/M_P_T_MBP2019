@echo off
setlocal

set "CERT=%~dp0AmtPtpDeviceUsbKm.cer"

if not exist "%CERT%" (
    echo [ERROR] Certificate "%CERT%" not found.
    pause
    exit /b 1
)

echo Importing certificate into Trusted Root...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CERT%' -CertStoreLocation Cert:\LocalMachine\Root | Out-Null"

echo Importing certificate into Trusted Publishers...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Certificate -FilePath '%CERT%' -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null"

echo.
echo Certificate imported successfully.
pause