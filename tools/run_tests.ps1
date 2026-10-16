# Builds and runs the host-side protocol tests, and verifies the ESP32 copies
# of the shared protocol files are identical to shared/.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'tools\build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$cc = $null
foreach ($name in 'gcc', 'clang') {
    if (Get-Command $name -ErrorAction SilentlyContinue) { $cc = $name; break }
}
if (-not $cc) { Write-Error 'No C compiler (gcc/clang) found.'; exit 1 }

$exe = Join-Path $build 'test_protocol.exe'
& $cc -std=c99 -Wall -Wextra -Werror -pedantic `
    -I (Join-Path $root 'shared') `
    (Join-Path $root 'tools\test_protocol.c') (Join-Path $root 'shared\protocol.c') `
    -o $exe
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed ($cc)."; exit 1 }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Error 'Tests failed.'; exit 1 }

$dest = Join-Path $root 'esp32\defuse_esp32\src\shared'
foreach ($f in 'protocol.h', 'protocol.c') {
    $a = Join-Path $root "shared\$f"
    $b = Join-Path $dest $f
    if (-not (Test-Path $b) -or (Get-FileHash $a).Hash -ne (Get-FileHash $b).Hash) {
        Write-Error "ESP32 copy of $f differs from shared/. Run tools\sync_shared.ps1."
        exit 1
    }
}
Write-Host 'OK: tests passed, ESP32 copies match shared/.'
