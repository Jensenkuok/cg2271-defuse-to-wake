# Hardware test: ESP32-S2 link, display, lights module and time sync

Goal: check the ESP32 side of the system on the bench with the MCXC444 running its default
firmware: the link, the OLED screens (including the lights module) and Wi-Fi/NTP time sync.
Rubric lines: ESP32 use, Comms protocol, Online (NTP), Task design, Sync.

**The gyro is disabled.** On 27 Oct the GY-521 board shorted the shared I2C bus and stopped the
OLED, so it is removed and `APP_USE_GYRO` is `0` in `config.h`: no gyro task is created, nothing
talks to address 0x68, and tamper detection is gone. The gyro code stays in the tree; section 3.3
(shake = strike) and the gyro lines in 3.7 apply only if a working gyro is fitted and `APP_USE_GYRO`
is set to `1`. The fourth sensor module is now the light sensor (section 3.2b).

## 0. Status of the code

The sketch compiles for `esp32:esp32:esp32s2` only when the four libraries below are installed.
They were not installed when this was written, so the firmware has **not** been compiled
against the real libraries. It was compiled against stand-in headers with the same API shape
(no errors or warnings in our own files). If the first real compile shows an API mismatch,
it will be in `display_task.cpp` (Adafruit GFX/SSD1306). The gyro needs no library (plain Wire).

Required libraries (Library Manager): **Adafruit SSD1306**, **Adafruit GFX Library**. The MPU-6050
is driven with plain Wire (no Adafruit MPU6050 library). ArduinoJson is not
used yet.

## 1. Wiring

| ESP32-S2 | Goes to | Notes |
|---|---|---|
| GPIO1 (Serial1 TX) | MCX **PTE23** (J4 pin 7, UART2 RX) | |
| GPIO2 (Serial1 RX) | MCX **PTE22** (J4 pin 5, UART2 TX) | |
| GND | MCX GND (J3 pin 4/12/14) | grounds **must** be tied |
| GPIO8 (SDA) | OLED SDA | the OLED is the only I2C device (no MPU-6050) |
| GPIO9 (SCL) | OLED SCL | |
| 3V3 | OLED VCC | **3.3 V** |
| GND | OLED GND | |

Address: OLED SSD1306 128x64 at **0x3C**. Do not connect a GY-521 to this bus.
Each board has its own USB cable. The MCX still has its own encoder, joystick and HC-SR04
wired as in `docs/hw_test_mcx_drivers.md`.

## 2. Build and flash

1. Install the four libraries above in the Arduino IDE.
2. Copy `esp32/defuse_esp32/secrets.example.h` to `secrets.h` (it is git-ignored) and fill in the
   two Wi-Fi hotspots. The other values are unused for now.
3. Board: **ESP32S2 Dev Module**, with the same upload options as in Lab 3 (the "USB CDC On
   Boot" choice decides whether `Serial` appears on the USB port or on the UART header).
4. Or compile from a terminal:

   ```
   "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn esp32:esp32:esp32s2:PartitionScheme=huge_app,PSRAM=enabled esp32/defuse_esp32
   ```

   Expect `0 errors`. Note the "Sketch uses ... bytes" line in the results table.
5. Upload, then open the Serial monitor at 115200.
6. Flash the MCX with the **default** build (`APP_DRIVER_TEST 0`, `APP_LOOPBACK_TEST 0`) and
   power it. The ESP32 firmware prints `[boot] Defuse to Wake ESP32-S2`.

### 2.1 PSRAM and flashing (ESP32-S2FNR2: 4 MB flash, 2 MB PSRAM)

Every HTTPS connection needs ~32 KB of internal RAM, so the big buffers (lesson lists, LLM replies,
web JSON, puzzle sets) live in PSRAM. Without PSRAM the HTTPS calls fail.

1. In the Arduino IDE set **Tools > PSRAM > Enabled**. The command-line equivalent is the
   `PSRAM=enabled` option in the FQBN above.
2. After a reset the first lines must show a non-zero PSRAM size, e.g.
   `[boot] start: PSRAM size=~2000000 free=... internal free=... largest=...`.
   `[boot] WARNING: PSRAM not enabled (Tools > PSRAM > Enabled)` means the board was flashed
   without PSRAM: re-flash. The 10 s status block repeats the numbers as
   `[mem] int_free=... int_largest=... int_min=... psram_free=...`. Expect `int_free` of at least
   60 KB and `int_largest` of at least 40 KB at idle (a TLS handshake needs the largest block).
3. The normal upload fails on this board (stub checksum error). Use the no-stub script, which
   compiles into `.esp32_build` and flashes the four images at 115200 baud:

   ```
   tools\flash_esp32.ps1 -Port COMx -Compile
   ```

   Without `-Compile` it flashes the last build. COMx is the board's USB port.

## 3. Tests

### 3.1 Link up with the MCX
Expect on the ESP32 Serial within a few seconds:

```
[boot] Defuse to Wake ESP32-S2
[display] OLED ready          (or: OLED not found: headless mode ...)
[link] UP
```

On the MCX console: the blue link-down blink stops once it hears the ESP32 heartbeat.
The 10 s `[status]` block shows `link=UP` and `retx`, `fail`, `crc`, `len` all 0.
OLED: the IDLE screen with a big clock (`--:--` until the time is valid) and two icons in the
corner (Wi-Fi bars, linked rings; the rings are crossed out when the link is down).
- PASS: `[link] UP`, no errors in `[link]`, the IDLE screen is shown, and there is no `[gyro]` line at all.

### 3.2 GAME_STATE screens while playing
Start a bomb with **BOOT** (the ESP32's GPIO0 button; Serial prints `[btn] BOOT pressed:
RING_NOW`) or with **SW3** on the MCX. Both ring in 3 s. Then check each screen:

| When | Expected OLED |
|---|---|
| armed, no alarm time known | the IDLE clock screen with a small bell under the link icon; the bottom line is `Alarm set` (or the next class if there is one) |
| armed, alarm set | the same clock screen as IDLE (big clock, IP, Wi-Fi and link icons) plus the bell. The bottom line swaps every 3 s between the next class (`Fri 11:00 CG2271 LT19`) and the alarm: `Alarm Fri 11:00` when it is 1 h or more away, `Rings in MM:SS` in the last hour. With no next class only the alarm text shows |
| bomb running, dial | top bar `03:00` counting down + 3 empty strike boxes; 4 digit boxes, the current one shows its value with a caret under it |
| after a right digit | that box shows `*`, the caret moves right |
| wrong digit | one strike box fills in; the digit resets to 0 |
| wires | 4 vertical wires, a cursor arrow under one; a cut wire is drawn broken |
| tripwire | horizontal 0-50 cm gauge with the band marked, a marker at the current distance, and a hold bar that fills over 3 s; the bottom line reads `Hold hand A-Bcm 3s` (the band of the BOMB_CONFIG, B = A + 10, A between 10 and 30; 15-25 only for the built-in default bomb before any config) |
| 2nd dial in one bomb (cannot happen in normal play now: every bomb already has the 4-module cap; the logic stays for a longer module list) | a different clue from the first dial: a built-in bank clue whose answer is the first code with each digit +3 (mod 10); entering that code clears the module, and a strike hint is built from it |
| lights | see section 3.2b |
| defused | `DEFUSED` and `TIME mm:ss` |
| exploded | `BOOM`, the screen flashes inverted about twice a second, then a new bomb starts |

Unplug the link wire for 5 s: a crossed-out link icon appears in the corner; replug and it goes
away. The MCX keeps running the game the whole time.
- PASS: every row matches, no stuck or garbled screen, the display follows inputs within about 0.2 s.

### 3.2b Lights module (the fourth module, replaces the gyro)
Every bomb is dial, wires, tripwire, lights. Wiring of the light sensor module (AO to PTB0 / J4[2], VCC to
3.3 V) is in `docs/pin_table.md` and `docs/hw_test_mcx_drivers.md` section 3.3b; do that calibration first.

**Flash both boards together.** The lights module is a new module type 4 in the protocol. An MCX with
the old firmware rejects a BOMB_CONFIG that contains it (the bomb never starts as configured), and an
ESP32 with the old firmware draws a `?` instead of the lights screen. After flashing, power-cycle both.

Start a bomb (BOOT button) and clear the dial, wires and tripwire. Expected on the lights screen:

| When | Expected OLED |
|---|---|
| module starts, MCX reading the room | top bar with the timer and strike boxes; title `LIGHTS ON`; an empty level bar; the bottom line `Calibrating...` (about 0.1-0.5 s, then it changes) |
| baseline taken | the level bar fills to the current room level; a thick tick marks the target on the bar; a line `lvl L  target T` under it; the bottom line `Turn on a light` |
| level reaches the target | the bottom line changes to `Hold it...` and the thin bar at the bottom starts filling |
| level stays at or above the target | the bottom bar is full after 2 s and the module clears (next screen is DEFUSED, since lights is the last module) |
| level dips below the target for under 0.3 s | the bottom bar keeps filling (forgiven) |
| level stays below the target for 0.3 s or more | the bottom bar empties, the line goes back to `Turn on a light` |

The bottom bar is estimated on the ESP32 from the received states (the MCX does not send it), so it can
lag the real hold by a state update; it never fills before the MCX would clear the module. Lights never
causes a strike.

Test with the room light: with the room lights on, cover the sensor with a finger when the module starts
(baseline low), then uncover it. PASS: the level rises above the tick, `Hold it...` appears and the module
clears after about 2 s.
Test with a phone torch: start the module in a bright room (or with the sensor uncovered), then hold a phone torch
about 5 cm from the sensor. PASS: the level jumps near 255, which is above the target (the target is never above 240),
the module clears after about 2 s. Moving the torch away before 2 s empties the hold bar after 0.3 s.

Serial check without an OLED (headless): the line reads `... LIGHTS level=87 target=130 hold=0% clue=[Turn on a light]`
(`calibrating` instead of the numbers before the baseline). The /eng page bomb panel shows `LIGHTS - module 4 of 4`
and `light level L, target T - needs more light` or `reached, hold it`.

### 3.3 Shake = strike (only with APP_USE_GYRO 1 and a working gyro; skip otherwise)
During a running bomb, shake the MPU-6050 board hard.
- ESP32 Serial: `[gyro] TAMPER sent <N> mg` (at most every 0.5 s).
- Every 5 s: `[gyro] peak deviation <N> mg in last 5 s (send >= 600, MCX strikes >= 1500) read_err=<E>`. Use
  this to choose better thresholds: `APP_TAMPER_SEND_MG` (ESP32) and `GAME_TAMPER_THRESHOLD_MG`
  (MCX).
- MCX: a strike appears only if `<N>` is 1500 or more, then there is a 2 s cooldown.
- A gentle tap that stays under 600 mg sends nothing. Shaking while the bomb is not running
  sends nothing.
- PASS: a hard shake adds exactly one strike per 2 s on the MCX console.

Gyro boot line and what it means (APP_USE_GYRO 1 only):
- `[gyro] chip at 0x68 WHO_AM_I=0x68 (MPU-6050) ready`: genuine MPU-6050.
- `WHO_AM_I=0x70` (or 0x71, 0x72, 0x98 ...) `(compatible clone) ready`, preceded by a WARNING line: a
  GY-521 clone (0x70 = MPU-6500). It works with the same registers; this is not an error.
- `WHO_AM_I` of 0x00 or 0xFF, or a failed read, counts as a failed init (floating or dead bus).
- `read_err=N` in the 5 s line counts samples whose I2C read failed (skipped, not used). A few on
  a marginal 100 kHz bus are tolerable; a number that grows by about 250 per line means the bus is down.
- If init fails the gyro retries every 5 s and logs the reason, e.g.
  `[gyro] MPU init failed at 0x68: WHO_AM_I read requestFrom got 0/1 (endTx=0); retrying every 5 s`.
  The first failure and any changed reason are always printed; an unchanged reason repeats at most once a minute.
  A loose wire or late power-up recovers without a reset. What the reason means:
  - `probe: no ACK`, or `endTx=2` (address NACK): wrong address or wiring (power, SDA/SCL, AD0).
  - `endTx=3` (data NACK), `endTx=5` (timeout), or `reset/wake/CONFIG/ACCEL_CONFIG write endTx=N`: flaky SDA/SCL joint or a solder bridge.
  - `WHO_AM_I=0x00` (stuck low) or `0xFF` (floating): SDA stuck low or not connected to the chip.
  - `requestFrom got N/1` with `endTx=0`: the read is cut off, usually a bad joint.
  - `bus busy (lock timeout)`: another task (OLED) held the I2C bus for over 30 ms.
- Serial command `mpu`: with `APP_USE_GYRO 0` it prints `[cmd] gyro disabled (APP_USE_GYRO 0)` and touches nothing. Enabled: read-only dump (no writes, safe while the gyro runs) for 0x68 and 0x69 of WHO_AM_I (0x75),
  PWR_MGMT_1 (0x6B), ACCEL_CONFIG (0x1C) and one raw accel sample (0x3B, three signed values), each with
  `[endTx=<code> got <n>/<want>]`. `--` means that read failed; read the codes as above. After a good init expect
  PWR_MGMT_1=0x01 and ACCEL_CONFIG=0x18; the accel Z value is about +-2048 when the board lies flat.

### 3.4 Wi-Fi and NTP time sync
With real hotspot details in `secrets.h` and the hotspot on:

```
[net] Wi-Fi connected
[net] time valid
[net] TIME_SYNC <unix seconds> queued
```

- The IDLE screen clock changes from `--:--` to the correct local time (UTC+8) and the Wi-Fi icon
  fills in.
- MCX 10 s stats: `[ctrl] accepted/rejected: time_sync=1/0`.
- Reset the MCX: `[link] DOWN` then `[link] UP`, and a new `TIME_SYNC ... queued` follows.
- Turn the hotspot off: `[net] Wi-Fi disconnected`, the icon empties, the game still works.
- PASS: correct clock, TIME_SYNC accepted by the MCX, no hang when Wi-Fi is missing.

### 3.5 OLED unplugged (headless mode)
Power off, unplug the OLED, power on.

```
[display] OLED not found: headless mode (state summary on Serial, 1 Hz)
[oled-off] IDLE link=up wifi=up clock=14:05
```

During a bomb the line shows the timer, strikes and the decoded module, for example
`[oled-off] BOMB_ACTIVE link=up wifi=up t=02:41 strikes=1/3 DIAL digit=2 val=7`.
- PASS: no crash or reboot, one summary line per second at most, link still works.

### 3.6 Health
In the 10 s `[status]` block:
- `[stack bytes free]`: every task above 512 bytes (this is bytes on the ESP32). With the gyro disabled there is no `gyro=` entry, and the /eng page system line says `gyro: disabled`.
- `heap_free` stays steady over 10 minutes (no steady decline).
- `[link] backpressure` and `lock_to` stay 0 or very small.

### 3.7 I2C scan (`i2c` command)
Type `i2c` in the serial monitor (also printed once at boot). It scans 0x08-0x77 on the shared bus and prints one line:
`[i2c] SDA=8 SCL=9 400000 Hz found: 0x3C 0x68`
- `0x3C` = OLED. The scan is read-only address probing of the whole bus; the gyro task never runs with `APP_USE_GYRO 0`, so nothing else talks to 0x68. If `0x68`/`0x69` shows up, a GY-521 is still connected: remove it.
- If nothing answers at 400 kHz, boot retries at 100 kHz and, if devices appear, keeps 100 kHz and says so in the line (long jumper wires).
- `found: none` = check the module power LED, solder joints, SDA/SCL swapped (GPIO8 = SDA, GPIO9 = SCL), then press RESET.
- PASS: only `0x3C` is listed.
- (APP_USE_GYRO 1 only) Only the OLED listed means the gyro wiring, power or solder is the problem; `[gyro] MPU init failed ... retrying every 5 s` in the boot log means the same, and a good gyro lists 0x3C and 0x68/0x69 with `[gyro] chip at 0x.. ready`.

## 4. Results (fill in)

| Check | Expected | Result | Notes |
|---|---|---|---|
| Sketch size / RAM (compile output) | (record) | | |
| `[link] UP` with the MCX | yes | | |
| IDLE screen with icons | yes | | |
| ARMED screen | yes | | |
| Dial screen follows the encoder | yes | | |
| Wires screen, cut wire broken | yes | | |
| Tripwire gauge, band, hold bar | yes | | |
| DEFUSED screen with time | yes | | |
| BOOM + inverted flash | yes | | |
| Link-down icon | yes | | |
| Lights screen: Calibrating..., level bar + target tick, Turn on a light | yes | | |
| Room light clears lights after about 2 s (Hold it... + hold bar) | yes | | |
| Phone torch clears lights; dip under 0.3 s forgiven, longer resets | yes | | |
| No `[gyro]` line, `mpu` prints "gyro disabled", I2C scan lists only 0x3C | yes | | |
| Wi-Fi connects | yes | | |
| NTP time correct on IDLE screen | yes | | |
| MCX accepts TIME_SYNC | `time_sync=1/0` | | |
| Headless mode without the OLED | yes | | |
| Min stack bytes free | > 512 | | |
