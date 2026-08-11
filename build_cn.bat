@echo off
title Floating Damage NG CN Build 1.50

echo ==========================================
echo Floating Damage NG 1.50 - Chinese Build
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
if exist "%DESKTOP_DIR%" (
    echo Auto-copying to %DESKTOP_DIR%...
    copy /Y "build\windows\x64\releasedbg\FloatingDamageNG.dll" "%DESKTOP_DIR%\" >nul && echo   DLL copied
    copy /Y "build\windows\x64\releasedbg\FloatingDamageNG.pdb" "%DESKTOP_DIR%\" >nul && echo   PDB copied
    copy /Y "Skyrim\Data\SKSE\Plugins\FloatingDamageNG.json" "%DESKTOP_DIR%\" >nul && echo   JSON copied
    echo Done.
) else (
    echo Desktop SKSE folder not found, copy manually:
    echo   DLL: build\windows\x64\releasedbg\FloatingDamageNG.dll
    echo   PDB: build\windows\x64\releasedbg\FloatingDamageNG.pdb
    echo   JSON: Skyrim\Data\SKSE\Plugins\FloatingDamageNG.json
)

pause
