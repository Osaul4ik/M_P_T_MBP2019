@echo off
setlocal
chcp 65001 >nul
title Windows Test Mode Manager

:: Check for Administrator privileges
net session >nul 2>&1
if errorlevel 1 (
    echo.
    echo This script must be run as Administrator.
    echo.
    pause
    exit /b
)

:: Detect current Test Mode status
set "STATE=DISABLED"
for /f "tokens=2" %%A in ('bcdedit /enum ^| findstr /i "testsigning"') do (
    if /i "%%A"=="Yes" set "STATE=ENABLED"
)

:MENU
cls
echo =====================================
echo        Windows Test Mode Manager
echo =====================================
echo.
echo Current status: %STATE%
echo.
echo [1] Enable Test Mode
echo [2] Disable Test Mode
echo [0] Exit
echo.

choice /c 120 /n /m "Select an option: "

if errorlevel 3 exit /b

if errorlevel 2 goto DISABLE
if errorlevel 1 goto ENABLE

:ENABLE
cls
echo Enabling Test Mode...
echo.

bcdedit /set testsigning on

if errorlevel 1 (
    echo Failed to enable Test Mode.
    echo.
    echo Possible reasons:
    echo  - Secure Boot is enabled.
    echo  - This script is not running as Administrator.
    echo.
    pause
    exit /b
)

echo Test Mode has been ENABLED successfully.
echo.
echo Please restart your computer for the changes to take effect.
echo.
pause
exit /b

:DISABLE
cls
echo Disabling Test Mode...
echo.

bcdedit /set testsigning off

if errorlevel 1 (
    echo Failed to disable Test Mode.
    echo.
    echo Possible reasons:
    echo  - Secure Boot is enabled.
    echo  - This script is not running as Administrator.
    echo.
    pause
    exit /b
)

echo Test Mode has been DISABLED successfully.
echo.
echo Please restart your computer for the changes to take effect.
echo.
pause
exit /b