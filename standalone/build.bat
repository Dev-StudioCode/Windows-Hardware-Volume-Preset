@echo off
setlocal
cd /d "%~dp0"

echo [1/2] Locating Clang/MinGW compiler...
set "COMPILER=clang++"
where clang++ >nul 2>&1
if errorlevel 1 (
    if exist "C:\Program Files\Windhawk\Compiler\bin\clang++.exe" (
        set "COMPILER=C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
    ) else (
        echo Error: clang++ was not found on PATH or in Windhawk directory.
        echo Please ensure Clang or MinGW-w64 is installed.
        pause
        exit /b 1
    )
)

echo [2/2] Compiling AudioHardwareVolumePreset.exe...
"%COMPILER%" --target=x86_64-w64-windows-gnu -O2 -mwindows main.cpp -lole32 -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 -o AudioHardwareVolumePreset.exe

if errorlevel 1 (
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo [SUCCESS] Build completed successfully: AudioHardwareVolumePreset.exe
pause
