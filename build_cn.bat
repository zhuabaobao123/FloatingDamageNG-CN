@echo off
title Floating Damage NG CN Build 1.51

echo ==========================================
echo Floating Damage NG 1.51 - Chinese Build
echo ==========================================
echo.

echo [1/3] Setting up VS environment...
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] VS 2026 not found!
    pause
    exit /b 1
)
echo [OK] VS 2026 ready
echo.

echo [2/3] Clearing cache...
cd /d E:\FloatingDamageNG-main1.50

if not exist C:\xmake-cache mkdir C:\xmake-cache
set XMAKE_GLOBALDIR=C:\xmake-data
set XMAKE_PKG_CACHEDIR=C:\xmake-cache

echo [3/3] Building (first time ~3-5 min)...
echo.

C:\xmake-bin\xmake.exe f -c -p windows --toolchain=msvc --vs=2026 -y
if errorlevel 1 (
    echo [WARN] Config attempt 1 failed, retrying...
    C:\xmake-bin\xmake.exe f -c -p windows --toolchain=msvc -y
    if errorlevel 1 (
        echo [ERROR] Configure failed!
        pause
        exit /b 1
    )
)

C:\xmake-bin\xmake.exe build -y
if errorlevel 1 (
    echo.
    echo ==========================================
    echo [ERROR] Build failed!
    echo ==========================================
    pause
    exit /b 1
)

echo.
echo ==========================================
echo [SUCCESS] Build complete!
echo ==========================================
echo.

set DESKTOP_DIR=%USERPROFILE%\Desktop\SKSE\Plugins
echo Auto-copying to %DESKTOP_DIR%...
if not exist "%DESKTOP_DIR%" mkdir "%DESKTOP_DIR%" >nul 2>&1
copy /Y "build\windows\x64\releasedbg\FloatingDamageNG.dll" "%DESKTOP_DIR%\" >nul && echo   DLL copied
copy /Y "build\windows\x64\releasedbg\FloatingDamageNG.pdb" "%DESKTOP_DIR%\" >nul && echo   PDB copied
copy /Y "Skyrim\Data\SKSE\Plugins\FloatingDamageNG.json" "%DESKTOP_DIR%\" >nul && echo   JSON copied
copy /Y "Skyrim\Data\SKSE\Plugins\FloatingDamageNG.ini" "%DESKTOP_DIR%\" >nul && echo   INI copied
xcopy /E /I /Y "Skyrim\Data\SKSE\Plugins\FloatingDamageNG\Fonts" "%DESKTOP_DIR%\FloatingDamageNG\Fonts" >nul && echo   Fonts copied
xcopy /E /I /Y "Skyrim\Data\SKSE\Plugins\FloatingDamageNG\Presets" "%DESKTOP_DIR%\FloatingDamageNG\Presets" >nul && echo   Presets copied
echo Done.

pause
