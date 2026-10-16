# Builds and runs the host-side tests (codec, link layer, game engine), and
# verifies the ESP32 copies of the shared files are identical to shared/.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'tools\build'
$shared = Join-Path $root 'shared'
$game = Join-Path $root 'game'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$cc = $null
foreach ($name in 'gcc', 'clang') {
    if (Get-Command $name -ErrorAction SilentlyContinue) { $cc = $name; break }
}
if (-not $cc) { Write-Error 'No C compiler (gcc/clang) found.'; exit 1 }

# test program -> sources it links besides the test file itself (relative to the repo root)
$programs = @(
    @{ Name = 'test_protocol'; Sources = @('shared\protocol.c') },
    @{ Name = 'test_link';     Sources = @('shared\protocol.c', 'shared\proto_link.c') },
    @{ Name = 'test_game';     Sources = @('shared\protocol.c', 'game\game.c') }
)
foreach ($p in $programs) {
    $exe = Join-Path $build "$($p.Name).exe"
    $srcs = @(Join-Path $root "tools\$($p.Name).c") + ($p.Sources | ForEach-Object { Join-Path $root $_ })
    & $cc -std=c99 -Wall -Wextra -Werror -pedantic -I $shared -I $game @srcs -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "Build of $($p.Name) failed ($cc)."; exit 1 }
    Write-Host "== $($p.Name)"
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "$($p.Name) failed."; exit 1 }
}

# Synced copies (tools/sync_shared.ps1) must be byte-identical to the canonical sources.
$copies = @(
    @{ From = 'shared'; To = 'esp32\defuse_esp32\src\shared'; Files = @('protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c') },
    @{ From = 'shared'; To = 'mcxc444\defuse_mcx\source\shared'; Files = @('protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c') },
    @{ From = 'game';   To = 'mcxc444\defuse_mcx\source\game'; Files = @('game_config.h', 'game.h', 'game.c') }
)
foreach ($c in $copies) {
    foreach ($f in $c.Files) {
        $a = Join-Path $root "$($c.From)\$f"
        $b = Join-Path $root "$($c.To)\$f"
        if (-not (Test-Path $b) -or (Get-FileHash $a).Hash -ne (Get-FileHash $b).Hash) {
            Write-Error "$($c.To)\$f differs from $($c.From)/. Run tools\sync_shared.ps1."
            exit 1
        }
    }
}
Write-Host 'OK: all tests passed, synced copies match shared/ and game/.'
