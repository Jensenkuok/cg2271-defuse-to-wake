# Hardware test: ESP32 AI puzzles, difficulty and BOMB_CONFIG

Goal: check on the bench that the ESP32 writes verified puzzles with the SoCLaaS LLM, falls back to
the template bank when the LLM is not available, and that every bomb the MCX starts gets a fresh
`BOMB_CONFIG` (puzzle answers + difficulty). Rubric lines: Online (GenAI + REST), ESP32 use,
Comms protocol (reliable `BOMB_CONFIG`), Sync, Task design.

## 0. Status of the code

- Host tests (`tools/run_tests.ps1`): JSON/`<think>` handling, puzzle validation, checker parsing,
  the template bank (every dial code 0000-9999 and every wire k of n solved by an independent
  solver), the difficulty rule, config ranges, pair selection. All pass.
- The firmware compiles for `esp32:esp32:esp32s2:PartitionScheme=huge_app` with no warnings in our
  files (numbers in section 5).
- **Not tested on hardware or against the real SoCLaaS yet**: this document is that test. The TLS
  certificate of the SoCLaaS host, the real latency and the way each model formats its JSON are the
  unknowns. If every call fails with `FAILED http (...)` before any HTTP status, see section 4.

New serial commands (115200, ESP32 Serial): `puzzles` (regenerate now), `ai` (print the stored set,
answers included: debug only), `diff` (print the difficulty rule applied to the stored mornings).

## 1. Setup

1. `secrets.h` needs `WIFI_SSID_*`/`WIFI_PASS_*` and `SOCLAAS_API_KEY`. The base URL and the model
   chains are in `src/app/config.h` (`APP_SOCLAAS_BASE_URL`, `APP_LLM_SETTER_CHAIN`,
   `APP_LLM_CHECKER_CHAIN`), not in `secrets.h`. Old `SOCLAAS_BASE_URL` / `SOCLAAS_MODEL_*`
   lines in your `secrets.h` are simply ignored.
2. Phone hotspot with internet (the API is on a public IP, no NUS network needed).
3. Flash the ESP32 sketch and the MCX default build. Open both serial consoles: ESP32 Serial (the
   `[ai]`, `[llm]`, `[cfg]`, `[btn]` lines) and the MCX debug console (the `[ctrl] accepted/rejected`
   line in its 10 s stats block).
4. First boot of a fresh board (nothing in flash) prints
   `[ai] no stored puzzle set: using template-bank puzzles until the first run`.

## 2. Tests

### 2.1 First run (`puzzles`)
Wait for `[net] Wi-Fi connected` and `[net] time valid`. A run starts by itself at boot because
there is no fresh set: `[ai] puzzle run wanted (boot: no fresh set)`. You can also type `puzzles`.
Expect lines like (latencies vary):

```
[ai] puzzle run wanted (boot: no fresh set)
[ai] generating 4 dial + 4 wire puzzles (setter x-test-2, checker x-test-1 first)
[llm] model=x-test-2 HTTP 200 2310 ms          <- setter call
[llm] model=x-test-1 HTTP 200 1780 ms          <- checker call (a DIFFERENT model)
[ai] D0 verified: x-test-2 wrote it, x-test-1 solved it (attempt 1)
...
[ai] set gen=2: 8 of 8 puzzles AI-verified, generated at unix 17...
[ai] D0 AI   verified   setter=x-test-2 checker=x-test-1 said=4271 tries=1 2310ms/1780ms ans=4271 "..."
...
[ai] new set stored in flash (8 AI + 0 bank puzzles)
```

A rejected attempt looks like `[ai] D2 attempt 1: x-test-1 answered 4270, expected 4271: rejected`
(then attempt 2 with a new riddle); after 3 failed attempts `[ai] D2: using a bank template after 3
attempt(s)`. A full run takes roughly 8 x (1-3 attempts) x 2 calls; the network task does one call
per pass, so the link and the alarm sync keep running meanwhile.

PASS: the run ends with `new set stored` and at least most puzzles `AI verified`; in every `[ai]`
line the checker model differs from the setter model; no line contains the key.

### 2.2 The listing (`ai`)
Type `ai`. Expect the same table as above plus `used mask 0x0, next pair 0`. Check by hand that
a few riddles really give the printed answer (dial code / wire number, wires counted from the left,
wire 1 = leftmost). PASS: the 4 dial riddles are at most 80 characters, the 4 wire riddles at most
60, none prints the 4-digit code literally.

### 2.3 Difficulty (`diff`)
Type `diff`. With no mornings stored: `-> 3 modules, 180 s: No history yet: standard bomb`. After
some real mornings (or after you defused with snoozes/strikes): the line explains the change, e.g.
`4 mod: 3 snoozes/7d` (3 or more snoozes in the last 7 mornings), `+30s: avg strikes>=2`,
`-30s: 3 clean` (the 3 newest mornings without a snooze). PASS: matches the rule in CLAUDE.md.

### 2.4 BOMB_CONFIG accepted by the MCX (demo ring)
Press the ESP32 **BOOT** button (GPIO0). Expect on the ESP32:

```
[btn] BOOT pressed: demo ring requested
[cfg] BOMB_CONFIG queued (demo ring): pair 0, 3 modules, 180s, 5 wires (cut 3), dial 4271 [No history yet: standard bomb]
[btn] RING_NOW queued
[cfg] BOMB_CONFIG ACKed: pair 0 is now the MCX's config for the next bomb
```

On the MCX console the next stats block shows `[ctrl] accepted/rejected: ... bomb_config=1/0`
(`accepted/rejected`; the rejected counter must stay 0). After 3 s the bomb starts: enter the
dial code from the `[cfg]` line (not the old default 4-2-7-1) and cut the printed wire. PASS:
`bomb_config=N/0` rises by one per config, the bomb uses the new code and wire, and with 4
modules (if `diff` says so) the 4th module is a second dial.

### 2.5 After an explosion: a different dial code
Start a bomb (BOOT), let it explode (3 wrong presses or let the timer run out). Within the 3 s
EXPLODED hold the ESP32 prints `[cfg] BOMB_CONFIG queued (explosion): pair 1, ...` and then
`ACKed`. The next bomb (+1 module) needs the NEW dial code and wire. Also expect
`[ai] pair 0 used, mask 0x1` when the first bomb started. PASS: the second bomb's code differs
from the first.

### 2.6 After a snooze
Long-press the encoder (snooze). Within a second: `[cfg] BOMB_CONFIG queued (snooze): pair N ...`.
The snoozed bomb (5 minutes later) uses that pair. PASS: same as 2.5.

### 2.7 All four pairs used
Run four bombs in a row (each explosion or each demo ring after a defuse uses the next pair). After
the 4th bomb starts: `[ai] pair 3 used, mask 0x0 (all pairs used: starting over)`; the next config
is pair 0 again. PASS: the log line appears and no config is missing.

### 2.8 Flash survives a reboot
Reset the ESP32. Expect `[ai] puzzle set loaded from flash: 8 AI-verified puzzles, ... used mask 0x...`
and no new run (the set is younger than 24 h). `ai` shows the same riddles. PASS: identical set.

### 2.9 Offline: the bank still works, the old set is kept
a) Stored set present: switch the phone hotspot off (or enable airplane mode on the hotspot phone).
   Type `puzzles`: `[ai] no Wi-Fi or clock: the cached set stays active (8 AI-verified puzzles)`.
   `ai` still shows the old set; press BOOT: a `BOMB_CONFIG` is queued and ACKed with it.
b) LLM unreachable but Wi-Fi up (wrong key, e.g. temporarily edit `SOCLAAS_API_KEY`): `puzzles`
   prints `[llm] model=... FAILED http HTTP 401` for each chain model, then
   `[ai] models unreachable: the rest of the set comes from the bank` and
   `[ai] no puzzle was verified by the models: the previous set stays active`. `ai` is unchanged.
c) Nothing stored and no Wi-Fi (erase flash or first boot, hotspot off): `[ai] no stored puzzle set:
   using template-bank puzzles ...`; BOOT still queues a `BOMB_CONFIG` whose riddles are from the
   bank (`ai` shows `BANK`). Bank riddles look like `1st=3; 2nd=1st+4; 3rd=2nd-5; 4th=1st*3`.
PASS: in all three cases the bomb still starts with a config the MCX accepts.

### 2.10 No LLM calls during a bomb
Type `puzzles` while a bomb is running: only `[cmd] puzzle run requested` is printed; the run waits
until the session ends. PASS: no `[llm]` / `[ai]` lines while the OLED shows ACTIVE.

## 3. Results

| Test | Result (PASS/FAIL) | Notes (models, latency, counts) |
|---|---|---|
| 2.1 first run: setter/checker differ, set stored | | |
| 2.2 `ai` listing, riddles correct by hand | | |
| 2.3 `diff` matches the rule | | |
| 2.4 BOOT: config accepted (`bomb_config=N/0`), new code works | | |
| 2.5 explosion: different dial code | | |
| 2.6 snooze: new config | | |
| 2.7 four pairs used, wrap-around | | |
| 2.8 set survives a reboot | | |
| 2.9a Wi-Fi off: old set kept | | |
| 2.9b LLM failing: bank fallback, old set kept | | |
| 2.9c nothing stored, offline: bank config works | | |
| 2.10 no LLM calls during a bomb | | |
| Verified puzzles in a run (of 8) / average calls | | |

## 4. Troubleshooting

- `FAILED http (connection refused|...)` and no status: TLS or network. The host is checked with the
  core's CA bundle. For a diagnosis only, set `APP_TLS_INSECURE 1` in `config.h` (encrypted, but the
  server is not authenticated), re-flash, and report whether it then works.
- `HTTP 401` = wrong key, `HTTP 404`/`400` = model ID no longer exists (SoCLaaS swaps models behind
  IDs: edit the chains in `config.h`).
- `FAILED empty`: the model spent all `max_tokens` (600) on reasoning. Raise `APP_LLM_MAX_TOKENS`.
- `did not return a usable {"puzzle"}`: the riddle was longer than 80 (wire: 60) characters or the
  JSON was broken; the attempt counts and the next one tries again.
- The BOOT button reaches the MCX through the network task: while one LLM call is in flight (up to
  30 s) the demo ring waits for it.

## 5. Build numbers

`arduino-cli compile --fqbn esp32:esp32:esp32s2:PartitionScheme=huge_app --warnings all`: 0 errors,
0 warnings in our files.

| | Value |
|---|---|
| Sketch (flash) | 1,176,488 bytes (37 % of 3,145,728) |
| Global variables (RAM) | 65,636 bytes (20 % of 327,680) |
| Your run of the same command | |
