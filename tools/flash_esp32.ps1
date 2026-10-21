# Builds (optional) and flashes the ESP32-S2 firmware with esptool in --no-stub mode.
# On this board the normal Arduino upload fails (stub checksum error); only --no-stub at 115200 works.
#   tools\flash_esp32.ps1 -Port COM5            # upload the last build from .esp32_build
#   tools\flash_esp32.ps1 -Port COM5 -Compile   # compile first, then upload
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [switch]$Compile
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $root 'esp32\defuse_esp32'
$build = Join-Path $root '.esp32_build'
$fqbn = 'esp32:esp32:esp32s2:PartitionScheme=huge_app,PSRAM=enabled'

$arduinoCli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$cliConfig = Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml'
$esptool = 'C:\Users\rkoh2\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe'
$bootApp0 = 'C:\Users\rkoh2\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.12\tools\partitions\boot_app0.bin'

# Runs a native command after printing it; stops the script if it fails.
function Invoke-Step([string]$exe, [string[]]$arguments) {
    Write-Host ('> "{0}" {1}' -f $exe, ($arguments -join ' '))
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Write-Error "$exe failed (exit $LASTEXITCODE)."; exit 1 }
}

if ($Compile) {
    Invoke-Step $arduinoCli @('--config-file', $cliConfig, 'compile', '--fqbn', $fqbn,
        '--build-path', $build, '--warnings', 'all', $sketch)
}

$bootloader = Join-Path $build 'defuse_esp32.ino.bootloader.bin'
$partitions = Join-Path $build 'defuse_esp32.ino.partitions.bin'
$app = Join-Path $build 'defuse_esp32.ino.bin'
foreach ($f in @($bootloader, $partitions, $bootApp0, $app, $esptool)) {
    if (-not (Test-Path $f)) { Write-Error "Missing $f (run with -Compile first?)."; exit 1 }
}

Invoke-Step $esptool @('--chip', 'esp32s2', '--port', $Port, '--baud', '115200', '--no-stub',
    '--before', 'default-reset', '--after', 'hard-reset',
    'write-flash', '-u', '--flash-mode', 'keep', '--flash-freq', 'keep', '--flash-size', 'keep',
    '0x1000', $bootloader, '0x8000', $partitions, '0xe000', $bootApp0, '0x10000', $app)
