# Builds and runs the host-side tests (codec + link layer), and verifies the
# ESP32 copies of the shared files are identical to shared/.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'tools\build'
$shared = Join-Path $root 'shared'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$cc = $null
foreach ($name in 'gcc', 'clang') {
    if (Get-Command $name -ErrorAction SilentlyContinue) { $cc = $name; break }
}
if (-not $cc) { Write-Error 'No C compiler (gcc/clang) found.'; exit 1 }

# test program -> extra sources it links besides the test file itself
$programs = @(
    @{ Name = 'test_protocol'; Sources = @('protocol.c') },
    @{ Name = 'test_link';     Sources = @('protocol.c', 'proto_link.c') }
)
foreach ($p in $programs) {
    $exe = Join-Path $build "$($p.Name).exe"
    $srcs = @(Join-Path $root "tools\$($p.Name).c") + ($p.Sources | ForEach-Object { Join-Path $shared $_ })
    & $cc -std=c99 -Wall -Wextra -Werror -pedantic -I $shared @srcs -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "Build of $($p.Name) failed ($cc)."; exit 1 }
    Write-Host "== $($p.Name)"
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "$($p.Name) failed."; exit 1 }
}

$dest = Join-Path $root 'esp32\defuse_esp32\src\shared'
foreach ($f in 'protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c') {
    $a = Join-Path $shared $f
    $b = Join-Path $dest $f
    if (-not (Test-Path $b) -or (Get-FileHash $a).Hash -ne (Get-FileHash $b).Hash) {
        Write-Error "ESP32 copy of $f differs from shared/. Run tools\sync_shared.ps1."
        exit 1
    }
}
Write-Host 'OK: all tests passed, ESP32 copies match shared/.'
