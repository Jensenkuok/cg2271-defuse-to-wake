# Copies the canonical shared/ and game/ sources into the board projects:
#   shared/ -> esp32/defuse_esp32/src/shared/   (Arduino only compiles files inside the sketch folder)
#   shared/ -> mcxc444/defuse_mcx/source/shared/
#   game/   -> mcxc444/defuse_mcx/source/game/
# Never edit the copies; edit shared/ or game/ and re-run this script.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$sharedFiles = 'protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c'
$gameFiles = 'game_config.h', 'game.h', 'game.c'

$targets = @(
    @{ From = 'shared'; To = 'esp32\defuse_esp32\src\shared'; Files = $sharedFiles },
    @{ From = 'shared'; To = 'mcxc444\defuse_mcx\source\shared'; Files = $sharedFiles },
    @{ From = 'game';   To = 'mcxc444\defuse_mcx\source\game'; Files = $gameFiles }
)
foreach ($t in $targets) {
    $dest = Join-Path $root $t.To
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($f in $t.Files) {
        Copy-Item -Path (Join-Path $root "$($t.From)\$f") -Destination (Join-Path $dest $f) -Force
    }
    Write-Host "Synced $($t.From)/ ($($t.Files -join ', ')) -> $($t.To)"
}
