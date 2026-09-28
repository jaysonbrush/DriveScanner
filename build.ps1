# Builds DriveScanner.exe. Requires Zig on PATH (or the ZIG environment variable).
$ErrorActionPreference = 'Stop'
$zig = if ($env:ZIG) { $env:ZIG } else { (Get-Command zig -ErrorAction SilentlyContinue).Source }
if (-not $zig) { throw 'Zig not found. Install it from https://ziglang.org/download/ and add it to PATH.' }
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path src\DriveScanner.ico)) { & .\tools\make-icon.ps1 }
    & $zig c++ -target x86_64-windows-gnu -Os -s -fno-exceptions -fno-rtti -nostdlib++ `
        -Wall -Wno-missing-field-initializers '-Wl,--subsystem,windows' `
        src\main.cpp src\DriveScanner.rc -o DriveScanner.exe `
        -ld2d1 -ldwrite -lcomctl32 -lshell32 -lmpr -lole32 -luser32 -lgdi32
    if ($LASTEXITCODE) { throw 'Build failed.' }
    Remove-Item DriveScanner.pdb -ErrorAction SilentlyContinue
} finally { Pop-Location }
