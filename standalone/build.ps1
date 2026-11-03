$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

Write-Host "[1/2] Locating Clang/MinGW compiler..." -ForegroundColor Cyan
$compiler = "clang++"
if (-not (Get-Command "clang++" -ErrorAction SilentlyContinue)) {
    $windhawkClang = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
    if (Test-Path $windhawkClang) {
        $compiler = $windhawkClang
    } else {
        Write-Error "clang++ was not found on PATH or in Windhawk directory."
        exit 1
    }
}

Write-Host "[2/2] Compiling AudioHardwareVolumePreset.exe..." -ForegroundColor Cyan
& $compiler --target=x86_64-w64-windows-gnu -O2 -mwindows main.cpp -lole32 -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 -o AudioHardwareVolumePreset.exe

if ($LASTEXITCODE -eq 0) {
    Write-Host "[SUCCESS] Build completed successfully: AudioHardwareVolumePreset.exe" -ForegroundColor Green
} else {
    Write-Error "Build failed with exit code $LASTEXITCODE"
}
