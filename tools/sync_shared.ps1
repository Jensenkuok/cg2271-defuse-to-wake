# Copies the canonical shared/protocol.{h,c} into the Arduino sketch folder
# (the Arduino IDE only compiles files inside the sketch folder).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'esp32\defuse_esp32\src\shared'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($f in 'protocol.h', 'protocol.c') {
    Copy-Item -Path (Join-Path $root "shared\$f") -Destination (Join-Path $dest $f) -Force
}
Write-Host "Synced shared/protocol.h and protocol.c -> $dest"
