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

# ESP32 pure-logic modules (C++11, no Arduino headers) built with g++.
$cxx = $null
foreach ($name in 'g++', 'clang++') {
    if (Get-Command $name -ErrorAction SilentlyContinue) { $cxx = $name; break }
}
if (-not $cxx) { Write-Error 'No C++ compiler (g++/clang++) found.'; exit 1 }
$app = Join-Path $root 'esp32\defuse_esp32\src\app'
$cxxPrograms = @(
    @{ Name = 'test_schedule'; Sources = @('esp32\defuse_esp32\src\app\schedule.cpp') },
    @{ Name = 'test_history';  Sources = @('esp32\defuse_esp32\src\app\history_ring.cpp', 'esp32\defuse_esp32\src\app\schedule.cpp') },
    # test_puzzle solves every 4-digit code by brute force: optimised build to keep it to seconds.
    @{ Name = 'test_puzzle';   Sources = @('esp32\defuse_esp32\src\app\puzzle_logic.cpp', 'esp32\defuse_esp32\src\app\history_ring.cpp',
                                           'esp32\defuse_esp32\src\app\schedule.cpp', 'shared\protocol.c'); Flags = @('-O2') },
    # test_text: OLED wrap/marquee, hints, summary, briefing/coach templates and LLM reply validation.
    @{ Name = 'test_text';     Sources = @('esp32\defuse_esp32\src\app\text_logic.cpp', 'esp32\defuse_esp32\src\app\puzzle_logic.cpp',
                                           'esp32\defuse_esp32\src\app\history_ring.cpp', 'esp32\defuse_esp32\src\app\schedule.cpp',
                                           'shared\protocol.c') },
    # test_web: settings validation, outage expiry, JSON writer/escaping, answer hiding (web page helpers).
    @{ Name = 'test_web';      Sources = @('esp32\defuse_esp32\src\app\web_logic.cpp', 'esp32\defuse_esp32\src\app\schedule.cpp') },
    # test_framelog: link frame log ring, names, filter and the corrupt-CRC fault helper.
    @{ Name = 'test_framelog'; Sources = @('esp32\defuse_esp32\src\app\frame_log.cpp', 'shared\protocol.c') },
    # test_page: the embedded web page (web_page.h): 20 KB budget, no external assets, no innerHTML, endpoints.
    @{ Name = 'test_page';     Sources = @() }
)
foreach ($p in $cxxPrograms) {
    $exe = Join-Path $build "$($p.Name).exe"
    $srcs = @(Join-Path $root "tools\$($p.Name).cpp") + @($p.Sources | ForEach-Object { Join-Path $root $_ })
    $flags = @()
    if ($p.Flags) { $flags = @($p.Flags) }
    & $cxx -std=c++11 -Wall -Wextra -Werror -pedantic @flags -I $app @srcs -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "Build of $($p.Name) failed ($cxx)."; exit 1 }
    Write-Host "== $($p.Name)"
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Error "$($p.Name) failed."; exit 1 }
}

# Synced copies (tools/sync_shared.ps1) must be byte-identical to the canonical sources.
$copies = @(
    @{ From = 'shared'; To = 'esp32\defuse_esp32\src\shared'; Files = @('protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c') },
    @{ From = 'shared'; To = 'mcxc444\defuse_mcx\source\shared'; Files = @('protocol.h', 'protocol.c', 'proto_link.h', 'proto_link.c') },
    @{ From = 'game';   To = 'mcxc444\defuse_mcx\source\game'; Files = @('game_config.h', 'game.h', 'game.c') },
    @{ From = 'game';   To = 'esp32\defuse_esp32\src\game'; Files = @('game_config.h') }
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
if (-not (Test-Path (Join-Path $root 'esp32\defuse_esp32\src\game\protocol.h'))) {
    Write-Error 'esp32 src/game/protocol.h shim missing. Run tools\sync_shared.ps1.'
    exit 1
}
Write-Host 'OK: all tests passed, synced copies match shared/ and game/.'
