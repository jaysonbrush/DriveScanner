# Builds scanner.exe with Zig (https://ziglang.org). Portable: set $env:ZIG or keep zig at C:\Tools\zig.
$ErrorActionPreference = 'Stop'
$zig = if ($env:ZIG) { $env:ZIG } else { 'C:\Tools\zig\zig.exe' }
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path src\scanner.ico)) { & .\tools\make-icon.ps1 }
    & $zig c++ -target x86_64-windows-gnu -Os -s -fno-exceptions -fno-rtti -nostdlib++ `
        -Wall -Wno-missing-field-initializers '-Wl,--subsystem,windows' `
        src\main.cpp src\scanner.rc -o scanner.exe `
        -ld2d1 -ldwrite -lcomctl32 -lshell32 -lmpr -lole32 -luser32 -lgdi32
    if ($LASTEXITCODE) { throw "build failed" }
    Remove-Item scanner.pdb -ErrorAction SilentlyContinue
    $size = (Get-Item scanner.exe).Length
    "scanner.exe built: {0:N0} bytes" -f $size
} finally { Pop-Location }
