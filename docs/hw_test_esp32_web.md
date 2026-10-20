# Hardware test: ESP32 local web page (My mornings + Engineering)

Goal: check on the bench that the device serves its own web page on the hotspot, that the page shows
the real state (alarm, classes, history, live bomb, link frames, AI puzzles), that the settings form
changes the alarm and rejects bad input, and that the two fault buttons demonstrate the link
protocol's recovery and the offline fallbacks without touching the game. Rubric lines: ESP32 use,
Comms protocol (frame log, NACK + retransmit), Online (fallbacks), Task design (own lowest-priority
task), Sync (copies under locks).

## 0. Status of the code

- Host tests (`tools/run_tests.ps1`): settings validation, the outage expiry rule (including
  `millis()` wraparound), the JSON writer and string escaping, the frame-log ring and its filter, the
  corrupt-CRC helper (the decoder must report a CRC error), the "hide the active answer" rule, and the
  page itself (size <= 20 KB, no external assets, no `innerHTML`, all endpoints present). All pass.
- The firmware compiles for `esp32:esp32:esp32s2:PartitionScheme=huge_app` with no warnings in our
  files (numbers in section 5).
- **Not tested on hardware yet**: this document is that test. The unknowns are mDNS on your phone,
  how the hotspot treats client-to-client traffic (AP isolation), and the real size of the status
  JSON on a busy day (the worst case computed on paper is about 7.8 KB of the 8 KB buffer).

What the page can do (all of it lives in the **web task**, priority 1 like the network task; the game,
the link and the display never wait on it):

| Route | What it does |
|---|---|
| `GET /` | the page, from flash (about 16 KB) |
| `GET /api/status` | one JSON object, polled every 500 ms |
| `GET/POST /api/settings` | read / save `nusmods_link` and `lead_min` |
| `POST /api/ring` | the BOOT-button demo ring (runs in the link task) |
| `POST /api/fault/corrupt` | arm "corrupt the CRC of the next reliable frame to the MCX" |
| `POST /api/fault/outage` | 60 s simulated internet outage |
| `POST /api/puzzles` | ask the network task for a puzzle run |

Two behaviours to know before you test:

- **Corrupt next packet** corrupts the next *reliable* frame (TIME_SYNC, SET_ALARM, BOMB_CONFIG,
  TAMPER_EVENT, CMD): only those are acknowledged and retransmitted, and the ESP32 sends a 1 Hz
  heartbeat that would otherwise always be "the next frame". The button stays `(armed...)` until
  such a frame is sent; **press Ring now** right after arming to send one at once. Set
  `APP_FAULT_CORRUPT_RELIABLE_ONLY 0` in `config.h` for the literal "next frame".
- **The frame log** lists the last 20 frames except the periodic ones (HEARTBEAT, GAME_STATE,
  TELEMETRY: dozens a second would push everything else out); errors are never filtered.
  An ACK / NACK row shows the sequence number being acknowledged (a NACK for a bad CRC shows `-`).

## 1. Setup

1. `secrets.h` as for the other tests (Wi-Fi, SoCLaaS key, Firebase). Nothing new is needed.
2. Phone hotspot with internet. Connect the **phone itself or a PC to the same hotspot** (a second
   device on it). Some hotspots isolate clients ("AP isolation"): then no device can reach the ESP32;
   use another hotspot.
3. Flash the ESP32 sketch and the MCX default build. Open the ESP32 Serial console and the MCX debug
   console.
4. After `[net] Wi-Fi connected` the ESP32 prints
   `[web] page at http://defuse.local or http://192.168.x.y (mDNS on)`. The same address is in the
   `[status] ... ip=` line every 10 s and on the OLED IDLE screen (small, **top-left corner**: the
   bottom row is the next-alarm line).
5. On the second device open `http://defuse.local` (iPhone, macOS, Windows 10+, Linux) or
   `http://<the IP>` (Android browsers usually cannot resolve `.local`).

## 2. Tests

### 2.1 The page loads, mornings tab
Open the page. PASS: it loads in about a second; the header shows the clock and the IP;
**My mornings** shows the next alarm (big time, date, class, venue, "rings in ..."), today's classes
(or "no classes"), tonight's bomb difficulty with its explanation, the briefing and coach text, the
streak chip and the table of up to 14 mornings (newest first), and the settings form filled with the
stored link and lead. Compare the alarm with the OLED IDLE bottom line and the Serial `[sched] next
alarm ...` line: same time.

### 2.2 Change `lead_min`, the alarm moves
In Settings change the lead from 60 to 90 and **Save**. PASS: the form says `Saved: 90 min lead;
timetable refresh started`; the Serial shows `[web] settings saved: lead 90 min`, `[cfg] settings
changed: timetable refresh + reschedule` and `[sched] next alarm ... lead 90 min`; the alarm card
moves 30 minutes earlier within a few seconds (the OLED too); with the MCX not in a bomb session a
`SET_ALARM` is sent. Change it back to 60.

### 2.3 Bad input is rejected
Try each and press Save; each must show a red message and change nothing (no `[web] settings saved`,
alarm unchanged):

| Input | Expected message contains |
|---|---|
| link `http://nusmods.com/timetable/sem-1/share?CG2271=LEC:01` | `must start with https://nusmods.com/timetable/` |
| link `https://example.com/timetable/x?CG2271=LEC:01` | same |
| link `https://nusmods.com/timetable/sem-1/share` (no modules) | `has no module selection` |
| lead `10`, lead `181` | `between 15 and 180` |
| lead `abc` | `must be a whole number` |

PASS: all rejected with HTTP 400 (browser DevTools > Network shows 400), stored values unchanged
after a reload of the page. Then save a valid link of your own and check the classes update after the
refresh.

### 2.4 Ring now starts a bomb within 1 s
With the MCX idle or armed press **Ring now**. PASS: within 1 s the Serial shows `[web] demo ring
requested`, `[cfg] BOMB_CONFIG queued (demo ring) ...`, `[btn] RING_NOW queued`; the OLED goes to
ARMED `RING IN 00:03`; the bomb starts 3 s later (the same path and delay as the BOOT button). While
a bomb is running **Ring now** answers `a bomb is already running` and does nothing; with the MCX
unplugged it answers `the MCX link is down`.

### 2.5 The Engineering tab follows the bomb
Open **Engineering** and run a bomb. PASS: the big timer, the strike dots, the module name with
`module n of N`, the puzzle text and (after one strike) the hint match the OLED, and a change on the
bomb (dial digit, cursor, cut wire, tripwire distance and hold) shows on the page within about
0.5 s (film the OLED and the phone together if you want a number). The link panel shows `UP`, the
frame counters, and rows such as `TX BOMB_CONFIG OK`, `RX ACK ACK`, `TX CMD OK`, newest first.

### 2.6 Answers are hidden while the bomb is armed or running
While the MCX is ARMED, during the bomb and the EXPLODED / DEFUSED holds the AI panel marks the pair
the MCX holds with an `ACTIVE` chip and shows `answer: hidden while armed/active` for **every**
cached puzzle (the other pairs are the ones the next bombs of the session will use, so they are
hidden too). Open `http://<IP>/api/status` directly: every `"answer"` is `"hidden"`. After the DEFUSED
hold ends and the MCX is IDLE again (no alarm armed) all answers are shown (engineering view).
PASS: you never see a dial code or wire of the bomb you are playing on the page.

### 2.7 Corrupt next packet: NACK, retransmit, bomb unaffected
1. On Engineering press **Corrupt next packet** and confirm. The button text says `(armed...)`.
2. Press **Ring now** (any reliable frame will do).
3. PASS: the frame log shows, newest first, something like:

   ```
   RX  ACK         5   ACK                      <- the retransmit was acknowledged
   TX  BOMB_CONFIG 5   RETX
   RX  NACK        -   NACK                     <- the MCX rejected the bad CRC
   TX  BOMB_CONFIG 5   CRC CORRUPTED (fault)    <- purple row
   ```

   The Serial prints `[fault] CRC of BOMB_CONFIG seq=5 corrupted on the wire ...`; the `retx`
   tile goes up by one; the MCX console shows its CRC error and NACK counters up by one; the button
   is back to `Corrupt next packet`; the bomb starts normally with the right puzzle and nothing
   else is lost. Repeat once while a bomb is running (arm, then wait for the next reliable frame such
   as a tamper event or press snooze via the encoder): the bomb is not affected.

### 2.8 Simulate internet outage and the offline fallbacks
1. Press **Simulate internet outage** and confirm. The AI line shows `SIMULATED OUTAGE nn s`
   counting down and the button's note shows `Outage active: nn s left`. Wi-Fi stays up: the page
   keeps updating and the OLED keeps the Wi-Fi icon.
2. During the outage press **Regenerate puzzles**. PASS on the Serial: `[web] puzzle run requested`,
   `[llm] model=x-test-2 FAILED simulated outage` for every chain model (no 10-30 s waits),
   `[ai] models unreachable: the rest of the set comes from the bank`, `[ai] D0: using a bank template
   ...`, `[ai] no puzzle was verified by the models: the previous set stays active`. The AI panel
   says `last run: 0 AI-verified, none verified, bank fallback, previous set kept` and the cached
   puzzles are unchanged.
3. Firebase queue: start a bomb with **Ring now**, press **Simulate outage** when you are on the
   last module, defuse it inside the 60 s. PASS: `Firebase pending` goes from 0 to 1 (Serial `[mirror]
   simulated outage: upload not attempted`, `[mirror] 1 pending, retry in 60 s`). When the countdown
   reaches 0: `[fault] simulated outage over: Firebase and timetable retry now`, then
   `[mirror] uploaded, 0 pending` within a few seconds, and the pending count drops to 0.
4. Optional: save a different lead during an outage. The alarm still moves (the timetable cache is
   used), the Serial shows `[tt] <module>: simulated outage` and, after the outage, the refresh runs
   at once.

### 2.9 The page never shows secrets
Open `http://<IP>/` and `http://<IP>/api/status`, `/api/settings` and look at the Serial log during
2.1-2.8. PASS: none contains the Wi-Fi password, the SoCLaaS key, the Firebase URL or auth value (search
the pages with the browser's find for the first characters of each). The NUSMods link is shown on
purpose (it is a user setting) but never printed to the Serial.

### 2.10 Connection lost banner
Switch the ESP32 off (unplug USB) or turn the second device's Wi-Fi off and on. PASS: a red
`Connection lost` banner appears within about a second and disappears by itself when the device
answers again; fetches time out after 2 s.

### 2.11 Cost to the rest of the system
Leave the page open on two devices for a few minutes (2 polls per second each) and run a bomb.
PASS: no `[link] SEND_FAILED`, the MCX stats show no rejected frames, `[stack bytes free]` shows
`link=` and `web=` well above 1000 and `heap_free` is stable, the OLED stays smooth. The Engineering
system line shows the same stack numbers.

### 2.12 IP on the OLED and mDNS
The OLED IDLE screen shows the IP small in the top-left corner once Wi-Fi is up (nothing before).
`ping defuse.local` from a PC on the hotspot answers (or the iPhone opens `http://defuse.local`).
PASS: the shown IP opens the page.

## 3. Results

| Test | Result (PASS/FAIL) | Notes |
|---|---|---|
| 2.1 page loads, mornings tab matches OLED/Serial | | |
| 2.2 lead 60 -> 90 moves the alarm | | |
| 2.3 bad link / bad lead rejected (400) | | |
| 2.4 Ring now: ARMED within 1 s, bomb after 3 s; refused while running | | |
| 2.5 Engineering follows the bomb within ~0.5 s | | |
| 2.6 answers hidden while armed/active, shown in IDLE | | |
| 2.7 corrupt next packet: CRC row, NACK, RETX, ACK; bomb unaffected | | |
| 2.8 outage: fail fast, bank fallback, Firebase pending grows then drains | | |
| 2.9 no secrets in pages or logs | | |
| 2.10 Connection lost banner | | |
| 2.11 no side effects (stacks, heap, link) | | |
| 2.12 IP on OLED, defuse.local | | |
| Page size / status size (DevTools Network) | | |

## 4. Troubleshooting

- The page does not load, the Serial shows the IP: AP isolation on the hotspot, or the device is on
  another Wi-Fi. Try the IP, not the `.local` name (Android), and the same hotspot.
- `503 status unavailable` and `[web] status not built (buffer X of 8192 bytes used)`: the JSON did
  not fit (very long link, many classes). Report the numbers; the response is never truncated.
- The page shows old data and `Connection lost`: the ESP32 is busy for seconds (a TLS handshake in the
  network task); the page recovers by itself.
- The Corrupt button stays `(armed...)`: no reliable frame was sent. Press Ring now.

## 5. Build numbers

`arduino-cli compile --fqbn esp32:esp32:esp32s2:PartitionScheme=huge_app --warnings all`: 0 errors,
0 warnings in our files.

| | Value |
|---|---|
| Sketch (flash) | 1,275,476 bytes (40 % of 3,145,728) |
| Global variables (RAM) | 91,620 bytes (27 % of 327,680); before the web page: 73,652 |
| Page (`WEB_PAGE`) | 16125 bytes (budget 20,480) |
| Web task stack | 8192 bytes (`APP_STACK_WEB`) |
| Your run of the same command | |
