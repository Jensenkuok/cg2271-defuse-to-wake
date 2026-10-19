# Hardware test: ESP32-S2 online features (timetable, alarm schedule, history, Firebase)

Goal: check that the ESP32 reads the NUSMods timetable, caches it in flash, schedules the next
alarm and sends it to the MCX, records every defused morning locally, and mirrors each record to
Firebase (write-only), with Wi-Fi coming and going.
Rubric lines: Online (REST + DB), ESP32 use, Task design, Comms protocol, Sync.
Prerequisite: `docs/hw_test_esp32_basic.md` passes (link, OLED, NTP time sync).

## 0. Status of the code

- Compiled with **0 errors and 0 warnings in our files** for the FQBN below. Host tests pass
  (`tools/run_tests.ps1`: `test_schedule`, `test_history` and the older ones).
- **Not yet run on hardware.** The NUSMods and Firebase HTTPS calls, the NVS flash writes and the
  TLS certificate check have only been compiled. If a request fails with a negative HTTP code, see
  section 4 ("TLS").

## 1. Build and flash

| Setting | Value |
|---|---|
| Libraries (Library Manager) | Adafruit SSD1306, Adafruit GFX, Adafruit MPU6050, **ArduinoJson 7.x** |
| Board | ESP32S2 Dev Module (`esp32:esp32:esp32s2`) |
| **Partition Scheme** | **Huge APP (3MB No OTA/1MB SPIFFS)** (`PartitionScheme=huge_app`) |
| FQBN for the command line | `esp32:esp32:esp32s2:PartitionScheme=huge_app` |

The online code grew the program to about 1.15 MB, which does not fit the default 1.2 MB app
partition with room to spare, so the Huge APP scheme is used. Changing the partition table needs a
full upload (the old NVS data is erased once).

```
"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" --config-file "%USERPROFILE%\.arduinoIDE\arduino-cli.yaml" compile --fqbn esp32:esp32:esp32s2:PartitionScheme=huge_app --warnings all esp32/defuse_esp32
```

`secrets.h` needs `WIFI_*`, `FIREBASE_DB_URL`, `FIREBASE_AUTH` and `NUSMODS_SHARE_LINK`. Use a
share link with at least one class in **semester 1** (the code keeps only `semester == 1`).
Connect the MCX (default firmware) and open the ESP32 Serial monitor at 115200.

### Serial test commands (type a line, press Enter)

| Command | Effect |
|---|---|
| `lead <min>` | set the wake-up lead (clamped to 15..180) |
| `link <url>` | save a new NUSMods share link (replaces the one from `secrets.h`) |
| `refresh` | fetch the timetable now |
| `resched` | recompute the alarm now |
| `hist` | print the stored mornings and the streak |

## 2. Tests

Clock check first: the IDLE screen must show the right local time (see the basic test). Every
schedule below needs a valid clock.

### 2.1 Timetable fetch
Power on with the hotspot on. After `[net] time valid`:

```
[tt] no timetable cache in flash          (first boot only)
[tt] CG2028: 3 lessons
[tt] CG2271: 4 lessons
[tt] GEN2061Y: 1 lessons
[tt] QF2104: 3 lessons
[tt] ST3131: 2 lessons
[tt] timetable updated: 13 lessons
```
The lesson counts depend on the slots in your link and on the weeks. Compare one module by hand
with nusmods.com. A module with no matching slot prints `0 lessons` (and if the total is 0 the
refresh is rejected and the old cache is kept).
- PASS: one line per module, a total, no `[tt] ... HTTP` or `JSON error` lines.

### 2.2 Cache survives a reboot with Wi-Fi off
Turn the hotspots **off**, then press the ESP32 reset button.
```
[tt] cache loaded: 13 lessons (fetched at unix 17....)
[hist] N mornings in flash
[mirror] N records waiting for Firebase
```
- PASS: the cache line appears with the same lesson count and there is no crash.
- Known limit: the ESP32 has no battery clock, so with **no** Wi-Fi at all the time stays invalid
  after a reboot and no schedule line / OLED line can be computed until SNTP works again. The MCX
  keeps an alarm it already has (it was not reset), so the alarm still rings.
- Then turn the hotspot on: the line `[sched] next alarm ...` appears once time is valid.

### 2.3 OLED shows the next alarm
On the IDLE screen the bottom line shows the wake-up time (class start minus the lead), the module
and the venue, for example `Fri 15:00 CG2271 LT19` for a 16:00 lecture with the default 60 min lead.
Serial prints the same moment:

```
[sched] next alarm Fri 2026-10-23 15:00 (unix 17....) for CG2271 LEC LT19, lead 60 min
```
Check against the timetable: the earliest class that starts at least `lead` + 1 min from now, within
7 days, skipping recess week (21-27 Sep) and anything after week 13.
- PASS: the OLED line and the Serial line agree with the real timetable.

### 2.4 SET_ALARM accepted on the MCX
After the `[sched]` line, Serial shows `TIME_SYNC ... queued` then `SET_ALARM <unix> queued` (always
in this order). The MCX 10 s stats show `[ctrl] accepted/rejected: time_sync=N/0 set_alarm=N/0 ...`
with N increased by one. The OLED goes from IDLE to `ARMED` with a `RING IN mm:ss` countdown.
- PASS: `set_alarm` accepted count rises, rejected stays 0, ARMED screen shows the countdown.
- If the next class is more than 99 minutes away the countdown wraps in `mm:ss` (display limit,
  harmless).

### 2.5 Changing the lead time reschedules
Type `lead 90`:
```
[cmd] lead_min = 90
[cfg] settings changed: timetable refresh + reschedule
[sched] next alarm Fri 2026-10-23 14:30 (unix ...) for CG2271 LEC LT19, lead 90 min
[tt] ... (a refresh runs too)
[net] TIME_SYNC ... queued
[net] SET_ALARM ... queued
```
The OLED line changes to the earlier time. Then `lead 5` prints `lead_min = 15` and `lead 999` prints
`lead_min = 180` (clamped). Reboot: the value stays (settings are in flash). Type `lead 60` to
restore.
- PASS: the alarm moves by exactly the change in lead, a new SET_ALARM is sent, the value persists.

### 2.6 A defuse writes a history record
Press BOOT (or SW3) to ring in 3 s and defuse the bomb. Optional: let one bomb explode first.
```
[result] outcome=1 duration=48s strikes=0 snoozes=0
[hist] DEFUSED recorded: duration=48s strikes=0 snoozes=0 explosions=0 module=-
[sched] ... (alarm recomputed; the demo ring is not tied to a lesson, so module is "-")
```
The MCX ignores SET_ALARM while it shows DEFUSED (30 s hold), so if the alarm changed, the
`TIME_SYNC` / `SET_ALARM queued` lines appear only after the OLED is back on IDLE.
After an explosion first, `explosions=1` and `strikes` is the sum over the exploded and the final
bomb. Type `hist`:
```
[hist] 1 mornings, streak 1
[hist]  #0 end=17.... dur=48s strikes=0 snoozes=0 explosions=0 module=-
```
Reset the ESP32: `[hist] 1 mornings in flash`. The 10 s `[online]` line shows `hist=1 streak=1`.
- PASS: one record per DEFUSED result, none for EXPLODED, the record survives a reset.

### 2.7 Firebase mirror
With Wi-Fi up, right after the record is written:
```
[mirror] uploaded, 0 pending
```
Open the Firebase console, Realtime Database, path `/mornings`: a new child with
`{ts, duration_s, strikes, snoozes, explosions, module, device:"defuse-1"}`. `ts` is the unix time
of the defuse. The auth value never appears in any Serial line.
- PASS: exactly one new entry per defuse, fields match the `hist` output.

### 2.8 Wi-Fi off, pending grows, Wi-Fi on, drains
1. Turn the hotspots off. Wait for `[net] Wi-Fi disconnected`.
2. Defuse two bombs. The 10 s line shows `[online] ... mirror_pending=2`.
3. Reset the ESP32 while the hotspot is still off: `[mirror] 2 records waiting for Firebase`
   (the queue is in flash).
4. Turn the hotspot on. Within about a minute (immediately after `[net] Wi-Fi connected` and time
   valid):
```
[mirror] uploaded, 1 pending
[mirror] uploaded, 0 pending
```
The console shows both entries, oldest first.
- PASS: pending count 2 then 0, no entry uploaded twice, no entry lost.
- Optional (back-off): put a wrong `FIREBASE_AUTH` in `secrets.h` and flash: expect
  `[mirror] upload failed: HTTP 401` then `[mirror] 1 pending, retry in 60 s`, then 120 s, 240 s,
  480 s, 600 s (the maximum). The entry stays queued.
- Optional (overflow): with the hotspot off defuse 15 times: `[mirror] queue full: dropped the oldest
  pending record` and pending stays 14.

### 2.9 Nightly refresh and settings change
- Leave the board on past 22:00 local: one `[tt] ...` fetch round runs that evening.
- `link <share link>` with a different set of classes: `[cfg] settings changed ...`, a refresh,
  a new `[sched]` line for the new classes.

### 2.10 Snooze is not overwritten
Rule: from the first BOMB_ACTIVE until the MCX is back in IDLE, the ESP32 sends **no** `SET_ALARM`
and **no** `CMD_CANCEL`, not even the re-send that normally follows a TIME_SYNC. Needs a valid
timetable so that an alarm exists (so `[sched] next alarm ...` was printed).
1. Press BOOT (or SW3) to ring in 3 s. Serial: `[sched] session started: no SET_ALARM / CANCEL until the MCX is IDLE`.
2. On the BOMB_ACTIVE screen snooze with a long press of the encoder (or `CMD_SNOOZE`). The OLED goes
   to ARMED with `RING IN 05:00`.
3. Make the ESP32 do its TIME_SYNC pass while snoozed: either wait for the next periodic
   `[net] TIME_SYNC ... queued` (every 10 min) or type `resched`. Note that the pass may only show a
   TIME_SYNC line (the MCX state is ARMED, so TIME_SYNC is still allowed).
4. Expected Serial: `[net] TIME_SYNC <unix> queued` **without** a `SET_ALARM` or `CMD_CANCEL` line after it.
   The MCX 10 s stats `set_alarm=N/0` does not increase.
5. Let the snooze run out: the bomb must ring again at +5 min (the countdown on the OLED was not reset
   or replaced by the timetable alarm).
6. Defuse it. After the DEFUSED hold the OLED is IDLE and Serial prints
   `[sched] session ended: alarm consumed, rescheduling`, then `[sched] next alarm ...` (the next
   day's first class when the ring was for today's first class) and `TIME_SYNC` + `SET_ALARM queued`.
- PASS: no `SET_ALARM` / `CMD_CANCEL` between "session started" and "session ended", the snooze re-rings
  at +5 min, and exactly one new `SET_ALARM` follows the session end.

### 2.11 Cleared alarm cancels
Rule: when the schedule has no alarm any more and the MCX is ARMED for one we sent, the ESP32 sends
`CMD_CANCEL` (outside a session only).
1. Make sure the MCX shows ARMED (an alarm was sent in 2.4) and no bomb has been running.
2. Make the schedule empty while the MCX is still ARMED: replace the share link (`link <url>`) with
   one whose only classes are more than 7 days away (for example a module with a single class in a
   later teaching week). A link that matches no lesson at all is rejected and the old cache kept, so
   it must match something. The settings change refreshes the timetable and reschedules.
3. Expected Serial: `[sched] no class in the next 7 days: alarm cleared`, then
   `[net] CMD_CANCEL queued (alarm cleared)`. The MCX goes from ARMED to IDLE (OLED IDLE screen).
4. Negative check: reset the ESP32 (nothing sent since boot) while the MCX is still ARMED for an old
   alarm and the schedule is empty: **no** `CMD_CANCEL` is sent (the ESP32 only cancels an alarm it
   sent itself). Restore the real link afterwards (`link <your share link>`).
5. Negative check 2: during a running session (bomb on screen, or snoozed ARMED) clear the schedule:
   no `CMD_CANCEL` is sent until the session has ended.
- PASS: exactly one `CMD_CANCEL`, MCX goes IDLE; no cancel after a reset or inside a session.

### 2.12 First class of the day only
With a timetable that has two classes on one day (for example the 12:00 and 16:00 classes on a Friday),
the `[sched] next alarm` line names the 12:00 class. After that alarm has rung (or its time has
passed), type `resched`: the alarm moves to the **next day's first class**, never to the 16:00 class.
- PASS: only one alarm per day, always for that day's earliest class.

### 2.13 The game does not depend on the network
Turn the hotspot off while a bomb is running. The game, the OLED and the link keep working. Network
retries (`[tt] will retry in 300 s`) only appear in the Serial log. Check that
`[stack bytes free] ... net=` stays above 512 and `lock_to` in the `[link]` line stays ~0.

## 3. Results (fill in)

| Check | Expected | Result | Notes |
|---|---|---|---|
| Sketch size / RAM (compile output) | 1151320 B (36%) / 55108 B (16%) | | record the new numbers |
| Lessons fetched per module | matches NUSMods | | |
| Cache loaded after reboot, Wi-Fi off | yes | | |
| OLED shows next alarm line | yes | | |
| `set_alarm` accepted on the MCX | +1 | | |
| `lead 90` moves the alarm by 30 min | yes | | |
| Lead clamp 15..180 | yes | | |
| DEFUSED writes a record | yes | | |
| History survives reset | yes | | |
| Firebase `/mornings` entry appears | yes | | |
| Offline: pending grows, then drains | 2 -> 0 | | |
| Min free stack of the network task | > 512 B | | |
| `heap_free` steady after several refreshes | yes | | |

## 4. TLS and troubleshooting

- Both HTTPS clients verify the server with the CA bundle built into the ESP32 core
  (`useBuiltinCACertBundle`). This needs a valid clock, so no request starts before SNTP is done.
- If a request fails with a negative code (`[tt] ...: HTTP -1` or `[mirror] upload failed: error -1`)
  and Wi-Fi is fine, a certificate problem is likely. As a **temporary diagnostic** set
  `APP_TLS_INSECURE` to `1` in `src/app/config.h`: the connection is still encrypted but the server
  is not authenticated. Report the result and set it back to `0`.
- `[tt] refresh failed: no lesson matched the selected slots`: the class numbers in the link do not
  exist in semester 1 of that module (check the link and the AY in `APP_NUSMODS_URL_PREFIX`).
- A lesson whose `weeks` is a date-range object instead of a list of week numbers is skipped.
