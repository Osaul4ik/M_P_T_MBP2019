@echo off
setlocal
chcp 65001 >nul
title Windows Test Mode Manager

:: Check for Administrator privileges. If not elevated yet, relaunch
:: this same script with a UAC prompt, preserving the working folder,
:: then exit this non-elevated instance.
net session >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator privileges...
    set "ELEVATE_VBS=%TEMP%\wsp_elevate_%RANDOM%.vbs"
    > "%ELEVATE_VBS%" echo On Error Resume Next
    >> "%ELEVATE_VBS%" echo Set UAC = CreateObject("Shell.Application")
    >> "%ELEVATE_VBS%" echo UAC.ShellExecute "%~f0", "", "%~dp0", "runas", 1
    cscript //nologo "%ELEVATE_VBS%"
    del "%ELEVATE_VBS%" >nul 2>&1
    exit /b
)

:: Detect current Test Mode status
:: Note: bcdedit's Yes/No value text can be localized on non-English
:: Windows builds, so we don't compare against "Yes"/"No". Instead we
:: rely on the fact that bcdedit only emits the "testsigning" element
:: at all when it has been explicitly set to on; when off (the
:: default), the element is absent from /enum output entirely. This
:: keeps detection correct regardless of display language.
set "STATE=DISABLED"
bcdedit /enum | findstr /i "testsigning" >nul
if not errorlevel 1 set "STATE=ENABLED"

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