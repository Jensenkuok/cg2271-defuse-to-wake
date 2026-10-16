# Builds the code zip for submission into dist/.
#   Interim -> B<SS>_<GG>_Interim_Code.zip      Final -> B<SS>_<GG>_Code.zip
# SS = lab slot, GG = group number, both zero-padded to 2 digits (slot 1 -> B01).
#
# Included: mcxc444/, esp32/, shared/, game/, tools/ (not tools/build/), docs/, README* in the root.
# Excluded: .git, dist/, build outputs (Debug/, Release/, .metadata/, tools/build/, *.o, *.exe), *.pdf.
#
# secrets.h: any file named secrets.h (case-insensitive) is EXCLUDED from the zip, with one
# informational line per excluded file. It never aborts the build, and the real file stays in place.
#
# SAFETY: the secret-content scan runs only on files that will be included in the zip. It aborts
# before creating any zip if one contains "Bearer " followed by a real-looking token, or "sk-"
# followed by 20+ key characters. Remove or scrub the offending file yourself; this script never
# edits files.
param(
    [Parameter(Mandatory = $true)][ValidateRange(1, 99)][int]$Slot,
    [Parameter(Mandatory = $true)][ValidateRange(1, 99)][int]$Group,
    [Parameter(Mandatory = $true)][ValidateSet('Interim', 'Final')][string]$Kind
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$ids = '{0:D2}_{1:D2}' -f $Slot, $Group
$zipName = if ($Kind -eq 'Interim') { "B${ids}_Interim_Code.zip" } else { "B${ids}_Code.zip" }

$includeDirs = 'mcxc444', 'esp32', 'shared', 'game', 'tools', 'docs'
$skipDirPattern = '(^|\\)(\.git|dist|Debug|Release|\.metadata)(\\|$)|^tools\\build(\\|$)'
$skipExtensions = '.pdf', '.o', '.exe'
$binaryExtensions = '.png', '.jpg', '.jpeg', '.gif', '.zip', '.bin', '.elf', '.axf', '.ico'
$tokenChars = 'A-Za-z0-9._~+/=\-'
$bearerPattern = "Bearer\s+([$tokenChars]{8,})"
$placeholderPattern = '^(<|\$|\{|%|\.\.\.|your|xxx|example|replace|placeholder|token|changeme)'
$skKeyPattern = 'sk-[A-Za-z0-9_\-]{20,}'

function Get-RelativePath([string]$full) {
    return $full.Substring($root.Length).TrimStart('\')
}

# Candidate files: everything under the include dirs plus README* in the root.
$candidates = New-Object System.Collections.Generic.List[System.IO.FileInfo]
foreach ($d in $includeDirs) {
    $p = Join-Path $root $d
    if (Test-Path $p) {
        Get-ChildItem -Path $p -Recurse -File -Force | ForEach-Object { $candidates.Add($_) }
    }
}
Get-ChildItem -Path $root -File -Force -Filter 'README*' | ForEach-Object { $candidates.Add($_) }

# Drop build outputs, PDFs and VCS/dist folders.
$candidates = $candidates | Where-Object {
    $rel = Get-RelativePath $_.FullName
    ($rel -notmatch $skipDirPattern) -and ($skipExtensions -notcontains $_.Extension.ToLower())
}

# Exclude secrets.h silently-but-visibly: one info line per file, never an abort.
$files = @()
foreach ($f in $candidates) {
    if ($f.Name -ieq 'secrets.h') {
        Write-Host ("Excluded (secrets.h): {0}" -f (Get-RelativePath $f.FullName)) -ForegroundColor Cyan
    } else {
        $files += $f
    }
}

# Content scan runs only on files that will actually be zipped.
$problems = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    $rel = Get-RelativePath $f.FullName
    if ($binaryExtensions -contains $f.Extension.ToLower() -or $f.Length -gt 5MB) { continue }
    $text = [System.IO.File]::ReadAllText($f.FullName, [System.Text.Encoding]::GetEncoding(28591))
    if ($text.IndexOf([char]0) -ge 0) { continue }
    foreach ($m in [regex]::Matches($text, $bearerPattern)) {
        if ($m.Groups[1].Value -inotmatch $placeholderPattern) {
            $problems.Add("${rel}: contains a Bearer token value")
            break
        }
    }
    if ([regex]::IsMatch($text, $skKeyPattern)) {
        $problems.Add("${rel}: contains an sk- style API key")
    }
}
if ($problems.Count -gt 0) {
    Write-Host 'ABORTED: possible secrets found. No zip was created.' -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Error 'Submission aborted by the secrets check.'
    exit 1
}

# Checks passed: build the zip with forward-slash entry names.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zipPath = Join-Path $dist $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in $files) {
        $entry = (Get-RelativePath $f.FullName).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $entry)
    }
}
finally {
    $zip.Dispose()
}
Write-Host ("Created {0} ({1} files, {2:N0} bytes)" -f $zipPath, @($files).Count, (Get-Item $zipPath).Length)
