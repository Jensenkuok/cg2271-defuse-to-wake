# Hardware test: ESP32 AI puzzles, difficulty and BOMB_CONFIG

Goal: check on the bench that the ESP32 writes verified puzzles with the SoCLaaS LLM, falls back to
the template bank when the LLM is not available, and that every bomb the MCX starts gets a fresh
`BOMB_CONFIG` (puzzle answers + difficulty). Rubric lines: Online (GenAI + REST), ESP32 use,
Comms protocol (reliable `BOMB_CONFIG`), Sync, Task design.

## 0. Status of the code

- Host tests (`tools/run_tests.ps1`): JSON/`<think>` handling, puzzle validation, checker parsing,
  the template bank (every dial code 0000-9999 and every wire k of n solved by an independent
  solver), the difficulty score and level table (`tools/test_difficulty.cpp`), config ranges, the
  tripwire band, pair selection. All pass.
- The firmware compiles for `esp32:esp32:esp32s2:PartitionScheme=huge_app,PSRAM=enabled` with no warnings in our
  files (numbers in section 5).
- **Not tested on hardware or against the real SoCLaaS yet**: this document is that test. The TLS
  certificate of the SoCLaaS host, the real latency and the way each model formats its JSON are the
  unknowns. If every call fails with `FAILED http (...)` before any HTTP status, see section 4.

New serial commands (115200, ESP32 Serial): `puzzles` (regenerate now), `ai` (print the stored set,
answers included: debug only), `diff` (print the difficulty mode, score, effective level and the bomb that level asks for),
`brief` (generate the morning briefing + coach message now, section 2.13).

**Who does what (changed):** `BOMB_CONFIG` triggers and the BOOT demo ring now run in the **link
task** (priority 4), not in the network task (priority 1). The network task can sit in an LLM call
for up to 30 s, and the MCX starts the next bomb 3 s after EXPLODED, so a fresh config sent from
there could miss that window. The link task never blocks on the network or on flash: it reads the
puzzle set under a short mutex hold, and the "used pair" flags it changes are written to flash by
the network task later (dirty flag). New texts on the OLED (puzzle riddle, hints, DEFUSED pages) are
in sections 2.11-2.13; the host tests for the text logic are `tools/test_text.cpp`.

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
[ai] generating 4 dial + 4 wire puzzles (setter qwen3.6:35b, checker x-test-1 first)
[llm] model=qwen3.6:35b HTTP 200 2310 ms          <- setter call
[llm] model=x-test-1 HTTP 200 1780 ms          <- checker call (a DIFFERENT model)
[ai] D0 verified: qwen3.6:35b wrote it, x-test-1 solved it (attempt 1)
...
[ai] set gen=2: 8 of 8 puzzles AI-verified, generated at unix 17...
[ai] D0 AI   verified   setter=qwen3.6:35b checker=x-test-1 said=4271 tries=1 2310ms/1780ms ans=4271 "..."
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
Type `diff`. With no mornings stored and the mode on Auto:

```
[diff] 0 mornings stored: 0 explosions, 0 snoozes
[diff] mode auto, score 50 -> level NORMAL
[diff] bomb: 3 modules, 180 s, 4-5 wires [Normal (Auto, score 50)]
```

The score starts at 50 and replays the stored mornings from the oldest to the newest: a fast clean
defuse (0 strikes, 0 snoozes, 0 explosions, 90 s or less) is +8, any other defuse 0, each explosion
-10, each snooze -5, clamped to 0..100 after every morning. 0-33 is EASY, 34-66 NORMAL, 67-100 HARD.
The level sets the bomb: EASY 240 s, 3 modules, 3-4 wires; NORMAL 180 s, 3 modules, 4-5 wires; HARD
150 s, 4 modules, 6 wires. PASS: after a few real mornings the printed score equals the rule above
(work it out from `hist`).

### 2.4 BOMB_CONFIG accepted by the MCX (demo ring)
Press the ESP32 **BOOT** button (GPIO0). Expect on the ESP32:

```
[btn] BOOT pressed: demo ring requested
[cfg] BOMB_CONFIG queued (demo ring): pair 0, 3 modules, 180s, 5 wires (cut 3), dial 4271, tripwire 20-30cm [Normal (Auto, score 50)]
[btn] RING_NOW queued
[cfg] BOMB_CONFIG ACKed: pair 0 is now the MCX's config for the next bomb
```

On the MCX console the next stats block shows `[ctrl] accepted/rejected: ... bomb_config=1/0`
(`accepted/rejected`; the rejected counter must stay 0). After 3 s the bomb starts: enter the
dial code from the `[cfg]` line (not the old default 4-2-7-1) and cut the printed wire. PASS:
`bomb_config=N/0` rises by one per config, the bomb uses the new code and wire, and with 4
modules (if `diff` says so) the 4th module is a second dial. The `[cfg]` line now also prints the
tripwire band, e.g. `tripwire 20-30cm`, and the difficulty text `[Normal (Auto, score 50)]`.

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

### 2.7b "Used" flags survive a reboot (deferred flash write)
After a bomb has started (`[ai] pair 0 used, mask 0x1`) wait a few seconds (the network task writes
the mask on its next pass), then reset the ESP32. Expect `used mask 0x1` in the
`puzzle set loaded from flash` line and `next pair 1` in `ai`. PASS: the mask is the one before the reset.

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

### 2.11 BOOT rings within 1 s, even during a `puzzles` run
Type `puzzles`, wait for the first `[llm] model=...` line (a call is in flight, up to 30 s), then
press BOOT at once. Expect within about a second, while the LLM call is still running:

```
[btn] BOOT pressed: demo ring requested
[cfg] BOMB_CONFIG queued (demo ring): pair N, ...
[btn] RING_NOW queued
[cfg] BOMB_CONFIG ACKed: ...
```

The bomb starts 3 s after the press (OLED ACTIVE). The puzzle run then pauses until the session
ends. Repeat once with no run going. PASS: both presses ring in 3 s +- 1 s; the `[cfg]` line comes
before `RING_NOW queued`; `bomb_config=N/0` on the MCX stats (no rejected config).

Also, the explosion case (2.5) is the other half of this fix: start `puzzles`, let a bomb explode
while a call is in flight. PASS: `BOMB_CONFIG queued (explosion)` appears within ~1 s of EXPLODED,
never after the next bomb has started.

### 2.12 OLED puzzle text and hints
Run a bomb (BOOT). Layout, size-1 font, 21 x 8 characters:

- **Dial module:** timer + strike boxes on top, the four digit boxes, then 3 rows of text: the dial
  riddle of the config the ESP32 sent (compare it with `ai`: the riddle of the pair in the
  `[cfg] BOMB_CONFIG queued` line). Over 63 characters the last row scrolls (about 5 characters per
  second).
- **Wires module:** the wires with their number 1..N under each (the cursor is the inverted
  number), then 2 rows with the wire riddle (the last one scrolls if it does not fit).
- **Tripwire:** the bottom line prints the band of the config, e.g. `Hold hand 20-30cm 3s` (section 2.14).

After an **explosion** the next bomb shows a different riddle (the next pair), also on the OLED.
Make **one strike** (wrong dial digit, or cut a wrong wire):

- dial: the last text row becomes `Hint: 4 2 _ _` (the first 2 digits; a second strike shows 3);
- wires: the last text row becomes `Hint: not 1` (the lowest-numbered wrong wire that is not cut
  yet) and that wire's number is shown as `X`.

The hint is gone in the next bomb (strikes are per bomb). With no OLED the same rows appear in the
1 Hz `[oled-off] ... text=[row / row / row]` line. PASS: riddle matches `ai`, hints appear only
after a strike, the scrolling row moves, the X is on a wrong wire.

### 2.13 DEFUSED pages and the briefing (`brief`)
After network + puzzles are up, with a timetable loaded (`link` set) type `brief`. Expect
(latencies vary):

```
[cmd] briefing run requested
[brief] generating for local day 20xxx: 3 classes, alarm Fri 07:30 (chain starts with qwen3.6:35b)
[llm] model=qwen3.6:35b HTTP 200 3120 ms
[brief] stored for local day 20xxx (briefing: LLM, coach: LLM, model qwen3.6:35b)
[brief] briefing: "First: CG2271 LEC 14:00 @LT19. Then ..."
[brief] coach:    "..."
```

Briefing is at most 120 characters, coach at most 80, plain ASCII. A run also starts by itself
after every finished puzzle run. If the model replies with something unusable the log says so and
the template fills the gap (`briefing: template`); `coach: template` is the deterministic line
from the history (streak, snoozes, average strikes).

Defuse a bomb **on the day the briefing is for** (set the lead time so the alarm is today, or use
BOOT after `brief` while the stored day is today). The DEFUSED screen (30 s) shows a title, three
page dots and rotates every 4 s: `Defused 2:31, 1 strike, 0 snoozes` -> the briefing -> the coach
message. Long text wraps to the 6 rows below the title; the last row scrolls if it does not fit.
If the stored text is for another day the screen shows the template built from today's timetable
(or `Good morning! Check your timetable.` with no clock/timetable). PASS: three pages in that
order, 4 s each; text readable; after a reboot the stored briefing is loaded
(`[brief] loaded from flash: for local day N`).
Also try a wrong key (2.9b): `brief` ends with `no model gave a usable text: the template is used`.

### 2.14 Difficulty mode, level -> bomb, moving tripwire band (P3)
1. Open `http://defuse.local/`. Under the lead slider there is a **Difficulty** list (Auto, Easy,
   Normal, Hard) and a muted line `Now: Normal (Auto, score 50)` (the effective level, the mode and the
   score). Choose **Hard** and press **Save**. PASS: the page says Saved and the line becomes
   `Now: Hard (manual, score 50)`; `http://defuse.local/eng` shows `HARD: 4 modules, 2:30`.
2. On the ESP32 Serial type `diff`. PASS: `mode hard, score 50 -> level HARD` and
   `bomb: 4 modules, 150 s, 6-6 wires`.
3. Within a few seconds of the Save (no session running) the Serial log shows the set being replaced
   from the offline bank, one puzzle per pass, then the normal AI run being requested:

   ```
   [ai] level changed (set was built for NORMAL, now HARD)
   [ai] refilling the set from the bank at level HARD
   [ai] bank set at level HARD installed
   [ai] puzzle run wanted (level changed)
   ```

   Wait for `installed`, then type `ai`: all 4 wire puzzles have 6 wires (`wires=6`). Type `puzzles`
   or wait for the run: AI puzzles are written at HARD.
4. Press **Ring now** (or BOOT). PASS: the `[cfg] BOMB_CONFIG queued` line says `4 modules, 150s, 6 wires`
   and `[Hard (manual, score 50)]`; the bomb has a 2:30 timer and the fourth module is a second dial;
   the OLED wires screen draws 6 wires.
5. Repeat with **Easy** (PASS: 240 s, 3 modules, 3 or 4 wires) and **Normal** (180 s, 3 modules, 4 or
   5 wires). Saving the mode that is already stored does nothing (no refill).
6. **Tripwire band:** ring several bombs in a row (let each explode or defuse) and reach the tripwire
   module each time. PASS: the OLED line reads `Hold hand A-Bcm 3s` with B = A + 10 and A between 10
   and 30 (never above 40 cm), a different band between bombs; the gauge marks that band; the
   `[cfg] ... tripwire A-Bcm` line on the ESP32 shows the same numbers; holding a hand inside that band
   for 3 s clears the module. The hold time and the 10 cm width never change with the level.
7. **No refill inside a bomb:** start a bomb, and while it runs save another mode on the page. PASS: no
   `refilling the set` line until the bomb is defused and the MCX is back in IDLE; then the refill
   runs.
8. **Auto:** choose Auto again. With a history of fast clean defuses the score climbs by 8 per morning;
   when it passes 66 the level (and after the morning ends, the cached set) becomes HARD. With
   snoozes/explosions it drops by 5/10 each.

## 3. Results

| Test | Result (PASS/FAIL) | Notes (models, latency, counts) |
|---|---|---|
| 2.1 first run: setter/checker differ, set stored | | |
| 2.2 `ai` listing, riddles correct by hand | | |
| 2.3 `diff` matches the rule (mode, score, level) | | |
| 2.4 BOOT: config accepted (`bomb_config=N/0`), new code works | | |
| 2.5 explosion: different dial code | | |
| 2.6 snooze: new config | | |
| 2.7 four pairs used, wrap-around | | |
| 2.8 set survives a reboot | | |
| 2.9a Wi-Fi off: old set kept | | |
| 2.9b LLM failing: bank fallback, old set kept | | |
| 2.9c nothing stored, offline: bank config works | | |
| 2.10 no LLM calls during a bomb | | |
| 2.11 BOOT rings in <= 1 s during a `puzzles` run; explosion config in time | | |
| 2.12 OLED riddle matches `ai`; hints after a strike; X on a wrong wire | | |
| 2.13 `brief` text valid; DEFUSED pages rotate 4 s each; stored text loads after reboot | | |
| 2.7b used mask survives a reboot | | |
| 2.14 mode on `/` saved, `diff` shows it, Ring now shows the level's time / modules / wires | | |
| 2.14 cached set refilled from the bank at the new level, not inside a bomb | | |
| 2.14 tripwire band moves between bombs (10-20 .. 30-40 cm), OLED shows the config's band | | |
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
- The BOOT button and the `BOMB_CONFIG` triggers run in the link task, so an LLM call in flight does
  not delay them. If the demo ring is still slow, look at the `[stack bytes free]` line of the 10 s
  status block: `link=` should stay well above 1000.
- To see what the real models do with the puzzle prompts without flashing, run
  `tools/soclaas_probe.ps1` on the PC (needs `$env:SOCLAAS_API_KEY`): per call it prints the model,
  HTTP status, latency, the first 200 characters of the raw content and the extracted JSON, then a
  summary table. Paste the table into the results above if a model behaves oddly.

## 5. Build numbers

`arduino-cli compile --fqbn esp32:esp32:esp32s2:PartitionScheme=huge_app,PSRAM=enabled --warnings all`: 0 errors,
0 warnings in our files.

| | Value |
|---|---|
| Sketch (flash) | 1,303,616 bytes (41 % of 3,145,728) |
| Global variables (RAM) | 61,220 bytes (18 % of 327,680) |
| Your run of the same command | |
