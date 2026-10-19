# SoCLaaS probe: run by the USER from a PC (NUS network or a phone hotspot) to see what the real
# models do with the ESP32's puzzle pipeline, without flashing anything.
#
#   $env:SOCLAAS_API_KEY = '<your key>'      # this shell only; the script never writes it anywhere
#   powershell -ExecutionPolicy Bypass -File tools\soclaas_probe.ps1
#
# It generates 3 dial + 2 wire puzzles exactly like esp32/defuse_esp32/src/app/puzzle_gen.cpp does:
# the script picks the answer, a SETTER model writes the riddle, a DIFFERENT CHECKER model solves it
# from the text alone, and the puzzle counts as verified only on an exact match (up to 3 attempts).
# Per LLM call it prints: model, HTTP status, latency, the first 200 characters of the raw content
# and the extracted JSON; per puzzle: verified yes/no. It ends with a summary table.
#
# KEEP IN SYNC with puzzle_gen.cpp (prompts, chains, temperatures, max_tokens, attempts) and with
# puzzle_logic.cpp (strip_think, extract_json_object, validate_puzzle, parse_checker_answer,
# checker_order). Chains and limits live in esp32/defuse_esp32/src/app/config.h.
param(
    [int]$DialCount = 3,
    [int]$WireCount = 2
)

$ErrorActionPreference = 'Stop'

$key = $env:SOCLAAS_API_KEY
if ([string]::IsNullOrWhiteSpace($key)) {
    Write-Host 'SOCLAAS_API_KEY is not set in this shell. Set it first:'
    Write-Host '  $env:SOCLAAS_API_KEY = ''<your key>'''
    Write-Host 'The script reads the key only from that variable and never stores or prints it.'
    exit 1
}

# ---- Settings (config.h: APP_SOCLAAS_*, APP_LLM_*, APP_PUZZLE_ATTEMPTS) ----
$Url = 'https://soclaas-api.comp.nus.edu.sg/v1/chat/completions'
$SetterChain  = @('x-test-2', 'x-test-1', 'qwen3.6:35b', 'default')     # APP_LLM_SETTER_CHAIN
$CheckerChain = @('x-test-1', 'x-test-2', 'qwen3.6:35b', 'default')     # APP_LLM_CHECKER_CHAIN
$MaxTokens    = 600      # APP_LLM_MAX_TOKENS
$SetterTemp   = 0.9      # APP_LLM_SETTER_TEMP
$CheckerTemp  = 0.0      # APP_LLM_CHECKER_TEMP
$TimeoutSec   = 30       # APP_LLM_TIMEOUT_S
$Attempts     = 3        # APP_PUZZLE_ATTEMPTS
$DialTextMax  = 80       # PUZZLE_DIAL_TEXT_MAX
$WireTextMax  = 60       # PUZZLE_WIRE_TEXT_MAX
$TextMin      = 10       # PUZZLE_TEXT_MIN
$RawShown     = 200

# ---- System prompts: KEEP IN SYNC with puzzle_gen.cpp (kSetterDialSys, kSetterWireSys, kCheckerDialSys, kCheckerWireFmt) ----
$SetterDialSys = 'You write one short logic puzzle for a bomb-defusal game. The user gives a secret 4-digit code. ' +
    'Write ONE puzzle of at most 80 characters, plain ASCII only, whose only solution is exactly that code ' +
    'and which pins down all four digits. Never write the code itself. ' +
    'Reply ONLY with JSON: {"puzzle":"..."}'
$SetterWireSys = 'You write one short riddle for a bomb-defusal game. The user gives the number of wires (numbered 1..N ' +
    'from the left) and the wire to cut. Write ONE riddle of at most 60 characters, plain ASCII only, whose ' +
    'only solution is that wire number. Never state the number directly. ' +
    'Reply ONLY with JSON: {"puzzle":"..."}'
$CheckerDialSys = 'Solve the puzzle. The answer is a 4-digit code (digits 0-9, leading zeros allowed). ' +
    'Reply ONLY with JSON: {"answer":"1234"}'
$CheckerWireFmt = 'Solve the puzzle. There are {0} wires numbered 1..{0} from the left. The answer is one wire number. ' +
    'Reply ONLY with JSON: {{"answer":"2"}}'

# ---- Text helpers (puzzle_logic.cpp) ----
function Strip-Think([string]$Text) {
    $t = [regex]::Replace($Text, '(?s)<think>.*?</think>', '')
    $i = $t.LastIndexOf('</think>')
    if ($i -ge 0) { $t = $t.Substring($i + 8) }                 # stray closing tag: drop everything before it
    $j = $t.IndexOf('<think>')
    if ($j -ge 0) { $t = $t.Substring(0, $j) }                  # unterminated opening tag: drop everything after it
    return $t.Trim()
}

# First balanced {...} (braces inside JSON strings do not count); $null if there is none.
function Extract-Json([string]$Text) {
    $t = Strip-Think $Text
    $start = $t.IndexOf('{')
    if ($start -lt 0) { return $null }
    $depth = 0
    $inStr = $false
    $esc = $false
    for ($k = $start; $k -lt $t.Length; $k++) {
        $c = $t[$k]
        if ($inStr) {
            if ($esc) { $esc = $false }
            elseif ($c -eq '\') { $esc = $true }
            elseif ($c -eq '"') { $inStr = $false }
        }
        elseif ($c -eq '"') { $inStr = $true }
        elseif ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') {
            $depth--
            if ($depth -eq 0) { return $t.Substring($start, $k - $start + 1) }
        }
    }
    return $null
}

function Get-JsonString([string]$Json, [string]$Name) {
    try { $o = $Json | ConvertFrom-Json } catch { return $null }
    $v = $o.$Name
    if ($null -eq $v) { return $null }
    return [string]$v
}

# validate_puzzle: length, printable ASCII, and for a dial the code must not appear literally.
function Test-Puzzle([string]$Text, [string]$Kind, [string]$Answer) {
    $max = $DialTextMax
    if ($Kind -eq 'wire') { $max = $WireTextMax }
    if ($Text.Length -lt $TextMin -or $Text.Length -gt $max) { return 'length ' + $Text.Length + ' not in ' + $TextMin + '..' + $max }
    if ($Text -notmatch '\A[\x20-\x7E]+\z') { return 'not printable ASCII' }
    if ($Kind -eq 'dial' -and $Text.Contains($Answer)) { return 'contains the code literally' }
    return $null
}

# parse_checker_answer: dial = exactly 4 digits (spaces ignored), wire = integer 1..wires.
function Read-CheckerAnswer([string]$Json, [string]$Kind, [int]$Wires) {
    $a = Get-JsonString $Json 'answer'
    if ($null -eq $a) { return $null }
    $a = $a.Trim()
    if ($Kind -eq 'dial') {
        $a = $a -replace ' ', ''
        if ($a -match '^[0-9]{4}$') { return $a }
        return $null
    }
    if ($a -match '^[0-9]+$') {
        $n = [int]$a
        if ($n -ge 1 -and $n -le $Wires) { return [string]$n }
    }
    return $null
}

# checker_order: every chain model except the setter's first, the setter's own model last.
function Get-CheckerOrder([string]$Setter) {
    $others = @($CheckerChain | Where-Object { $_ -ne $Setter })
    $same = @($CheckerChain | Where-Object { $_ -eq $Setter })
    return @($others + $same)
}

# ---- HTTP ----
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Net.Http
$Client = New-Object System.Net.Http.HttpClient
$Client.Timeout = [TimeSpan]::FromSeconds($TimeoutSec)

$Calls = New-Object System.Collections.ArrayList     # every call, for the per-model table

function Invoke-Chat([string]$Role, [string]$Model, [string]$System, [string]$User, [double]$Temp) {
    $body = @{
        model       = $Model
        max_tokens  = $MaxTokens
        temperature = $Temp
        messages    = @(
            @{ role = 'system'; content = $System },
            @{ role = 'user'; content = $User }
        )
    } | ConvertTo-Json -Depth 5 -Compress
    $status = 0
    $content = ''
    $problem = ''
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $req = New-Object System.Net.Http.HttpRequestMessage ([System.Net.Http.HttpMethod]::Post), $Url
        $req.Headers.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue 'Bearer', $key
        $req.Content = New-Object System.Net.Http.StringContent $body, ([Text.Encoding]::UTF8), 'application/json'
        $resp = $Client.SendAsync($req).GetAwaiter().GetResult()
        $status = [int]$resp.StatusCode
        $text = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if ($status -eq 200) {
            $c = ($text | ConvertFrom-Json).choices[0].message.content
            if ($null -ne $c) { $content = [string]$c }
        }
    }
    catch {
        $problem = $_.Exception.Message
        if ($_.Exception.InnerException) { $problem = $_.Exception.InnerException.Message }
    }
    $sw.Stop()
    $ms = [int]$sw.ElapsedMilliseconds
    $clean = Strip-Think $content
    $ok = ($status -eq 200 -and $clean.Length -gt 0)
    $json = $null
    if ($ok) { $json = Extract-Json $content }

    $raw = ($content -replace '[\r\n]+', ' ')
    if ($raw.Length -gt $RawShown) { $raw = $raw.Substring(0, $RawShown) }
    $statusText = [string]$status
    if ($status -eq 0) { $statusText = 'no response' }
    Write-Host ('  [{0}] model={1} HTTP {2} {3} ms' -f $Role, $Model, $statusText, $ms)
    if ($problem) { Write-Host ('      error: ' + $problem) }
    if ($status -eq 200 -and -not $ok) { Write-Host '      empty content (only <think> text, or max_tokens used up by reasoning)' }
    if ($content) { Write-Host ('      raw : ' + $raw) }
    if ($json) { Write-Host ('      json: ' + $json) } elseif ($ok) { Write-Host '      json: (none found)' }

    [void]$Calls.Add([pscustomobject]@{ Model = $Model; Role = $Role; Ok = $ok; Status = $status; Ms = $ms })
    return [pscustomobject]@{ Ok = $ok; Status = $status; Ms = $ms; Json = $json }
}

# ---- One puzzle: setter chain, validation, checker chain, exact match ----
function New-Puzzle([string]$Kind, [int]$Index) {
    if ($Kind -eq 'dial') {
        $answer = (-join (1..4 | ForEach-Object { Get-Random -Minimum 0 -Maximum 10 }))
        $wires = 0
    }
    else {
        $wires = Get-Random -Minimum 3 -Maximum 7              # 3..6
        $answer = [string](Get-Random -Minimum 1 -Maximum ($wires + 1))
    }
    Write-Host ''
    Write-Host ('== {0} {1}: answer {2}{3}' -f $Kind.ToUpper(), $Index, $answer, $(if ($wires) { " ($wires wires)" } else { '' }))

    $res = [pscustomobject]@{
        Kind = $Kind; Answer = $answer; Verified = $false; Setter = '-'; Checker = '-'; Said = '-'
        Attempts = 0; SetterMs = 0; CheckerMs = 0; Text = ''
    }
    for ($attempt = 1; $attempt -le $Attempts -and -not $res.Verified; $attempt++) {
        $res.Attempts = $attempt
        Write-Host ('  -- attempt {0}' -f $attempt)
        if ($Kind -eq 'dial') {
            $sys = $SetterDialSys
            $user = 'Code: ' + $answer
        }
        else {
            $sys = $SetterWireSys
            $user = 'Wires: ' + $wires + '. Cut wire: ' + $answer
        }
        # Setter chain: the first model that answers at all is used (a failing model hands over).
        $text = $null
        $setter = $null
        $setterMs = 0
        foreach ($m in $SetterChain) {
            $r = Invoke-Chat 'setter ' $m $sys $user $SetterTemp
            if ($r.Ok) {
                $setter = $m
                $setterMs = $r.Ms
                if ($r.Json) { $text = Get-JsonString $r.Json 'puzzle' }
                break
            }
        }
        if ($null -eq $setter) { Write-Host '  no setter model answered'; continue }
        if ($null -eq $text) { Write-Host '  -> no usable {"puzzle"} in the reply: attempt failed'; continue }
        $bad = Test-Puzzle $text $Kind $answer
        if ($bad) { Write-Host ('  -> riddle rejected by validation (' + $bad + '): "' + $text + '"'); continue }
        Write-Host ('  riddle: "' + $text + '"')
        $res.Text = $text
        $res.Setter = $setter
        $res.SetterMs = $setterMs

        # Checker chain: a different model first; it sees ONLY the riddle, never the answer.
        if ($Kind -eq 'dial') { $csys = $CheckerDialSys } else { $csys = [string]::Format($CheckerWireFmt, $wires) }
        foreach ($m in (Get-CheckerOrder $setter)) {
            $r = Invoke-Chat 'checker' $m $csys $text $CheckerTemp
            if (-not $r.Ok) { continue }
            $got = $null
            if ($r.Json) { $got = Read-CheckerAnswer $r.Json $Kind $wires }
            if ($null -eq $got) { Write-Host '      no usable answer from this model'; continue }
            $res.Checker = $m
            $res.Said = $got
            $res.CheckerMs = $r.Ms
            if ($got -eq $answer) { $res.Verified = $true }
            break
        }
        Write-Host ('  verified: ' + $(if ($res.Verified) { 'YES' } else { 'no (checker said ' + $res.Said + ', expected ' + $answer + ')' }))
    }
    return $res
}

# ---- Run ----
Write-Host ('SoCLaaS probe: {0} dial + {1} wire puzzles, setter chain {2}, checker chain {3}' -f $DialCount, $WireCount, ($SetterChain -join ' > '), ($CheckerChain -join ' > '))
$results = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $DialCount; $i++) { [void]$results.Add((New-Puzzle 'dial' $i)) }
for ($i = 0; $i -lt $WireCount; $i++) { [void]$results.Add((New-Puzzle 'wire' $i)) }

# ---- Summary ----
Write-Host ''
Write-Host '=== Summary: puzzles ==='
$n = 0
$table = $results | ForEach-Object {
    [pscustomobject]@{
        '#'        = $n++
        Kind       = $_.Kind
        Answer     = $_.Answer
        Setter     = $_.Setter
        Checker    = $_.Checker
        Said       = $_.Said
        Verified   = $(if ($_.Verified) { 'yes' } else { 'no' })
        Attempts   = $_.Attempts
        'SetterMs' = $_.SetterMs
        'CheckerMs' = $_.CheckerMs
    }
}
($table | Format-Table -AutoSize | Out-String).TrimEnd() | Write-Host
$verified = @($results | Where-Object { $_.Verified }).Count
Write-Host ('Verified: {0} of {1}' -f $verified, $results.Count)

Write-Host ''
Write-Host '=== Summary: models ==='
$perModel = $Calls | Group-Object Model | ForEach-Object {
    $okCalls = @($_.Group | Where-Object { $_.Ok })
    $avg = 0
    if ($okCalls.Count -gt 0) { $avg = [int](($okCalls | Measure-Object Ms -Average).Average) }
    [pscustomobject]@{
        Model   = $_.Name
        Calls   = $_.Count
        Ok      = $okCalls.Count
        Failed  = $_.Count - $okCalls.Count
        'AvgMsOk' = $avg
    }
}
($perModel | Format-Table -AutoSize | Out-String).TrimEnd() | Write-Host
Write-Host ('Total LLM calls: ' + $Calls.Count)
