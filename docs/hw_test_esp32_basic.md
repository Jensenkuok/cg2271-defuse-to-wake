# Hardware test: ESP32-S2 link, display, gyro and time sync

Goal: check the ESP32 side of the system on the bench with the MCXC444 running its default
firmware: the link, the OLED screens, tamper detection and Wi-Fi/NTP time sync.
Rubric lines: ESP32 use, Comms protocol, Online (NTP), Task design, Sync.

## 0. Status of the code

The sketch compiles for `esp32:esp32:esp32s2` only when the four libraries below are installed.
They were not installed when this was written, so the firmware has **not** been compiled
against the real libraries. It was compiled against stand-in headers with the same API shape
(no errors or warnings in our own files). If the first real compile shows an API mismatch,
it will be in `display_task.cpp` (Adafruit GFX/SSD1306) or `gyro_task.cpp` (Adafruit MPU6050).

Required libraries (Library Manager): **Adafruit SSD1306**, **Adafruit GFX Library**,
**Adafruit MPU6050** (pulls in Adafruit Unified Sensor and Adafruit BusIO). ArduinoJson is not
used yet.

## 1. Wiring

| ESP32-S2 | Goes to | Notes |
|---|---|---|
| GPIO1 (Serial1 TX) | MCX **PTE23** (J4 pin 7, UART2 RX) | |
| GPIO2 (Serial1 RX) | MCX **PTE22** (J4 pin 5, UART2 TX) | |
| GND | MCX GND (J3 pin 4/12/14) | grounds **must** be tied |
| GPIO8 (SDA) | OLED SDA and MPU-6050 SDA | same I2C bus |
| GPIO9 (SCL) | OLED SCL and MPU-6050 SCL | |
| 3V3 | OLED VCC and MPU-6050 VCC | **3.3 V** |
| GND | OLED GND and MPU-6050 GND | |

Addresses: OLED SSD1306 128x64 at **0x3C**, MPU-6050 (GY-521) at **0x68** (AD0 pin low).
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
   "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn esp32:esp32:esp32s2 esp32/defuse_esp32
   ```

   Expect `0 errors`. Note the "Sketch uses ... bytes" line in the results table.
5. Upload, then open the Serial monitor at 115200.
6. Flash the MCX with the **default** build (`APP_DRIVER_TEST 0`, `APP_LOOPBACK_TEST 0`) and
   power it. The ESP32 firmware prints `[boot] Defuse to Wake ESP32-S2`.

## 3. Tests

### 3.1 Link up with the MCX
Expect on the ESP32 Serial within a few seconds:

```
[boot] Defuse to Wake ESP32-S2
[display] OLED ready          (or: OLED not found: headless mode ...)
[gyro] MPU-6050 ready
[link] UP
```

On the MCX console: the blue link-down blink stops once it hears the ESP32 heartbeat.
The 10 s `[status]` block shows `link=UP` and `retx`, `fail`, `crc`, `len` all 0.
OLED: the IDLE screen with a big clock (`--:--` until the time is valid) and two icons in the
corner (Wi-Fi bars, linked rings; the rings are crossed out when the link is down).
- PASS: `[link] UP`, no errors in `[link]`, the IDLE screen is shown.

### 3.2 GAME_STATE screens while playing
Start a bomb with **BOOT** (the ESP32's GPIO0 button; Serial prints `[btn] BOOT pressed:
RING_NOW`) or with **SW3** on the MCX. Both ring in 3 s. Then check each screen:

| When | Expected OLED |
|---|---|
| waiting | `ARMED` (big) and `waiting...` (no alarm time stored yet) |
| bomb running, dial | top bar `03:00` counting down + 3 empty strike boxes; 4 digit boxes, the current one shows its value with a caret under it |
| after a right digit | that box shows `*`, the caret moves right |
| wrong digit | one strike box fills in; the digit resets to 0 |
| wires | 4 vertical wires, a cursor arrow under one; a cut wire is drawn broken |
| tripwire | horizontal 0-50 cm gauge with the 18-22 cm band marked, a marker at the current distance, and a hold bar that fills over 3 s |
| bottom line | `Clue: (AI clue pending)` (placeholder) |
| defused | `DEFUSED` and `TIME mm:ss` |
| exploded | `BOOM`, the screen flashes inverted about twice a second, then a new bomb starts |

Unplug the link wire for 5 s: a crossed-out link icon appears in the corner; replug and it goes
away. The MCX keeps running the game the whole time.
- PASS: every row matches, no stuck or garbled screen, the display follows inputs within about 0.2 s.

### 3.3 Shake = strike
During a running bomb, shake the MPU-6050 board hard.
- ESP32 Serial: `[gyro] TAMPER sent <N> mg` (at most every 0.5 s).
- Every 5 s: `[gyro] peak deviation <N> mg in last 5 s (send >= 600, MCX strikes >= 1500)`. Use
  this to choose better thresholds: `APP_TAMPER_SEND_MG` (ESP32) and `GAME_TAMPER_THRESHOLD_MG`
  (MCX).
- MCX: a strike appears only if `<N>` is 1500 or more, then there is a 2 s cooldown.
- A gentle tap that stays under 600 mg sends nothing. Shaking while the bomb is not running
  sends nothing.
- PASS: a hard shake adds exactly one strike per 2 s on the MCX console.

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
- PASS: no crash or reboot, one summary line per second at most, link and gyro still work.

### 3.6 Health
In the 10 s `[status]` block:
- `[stack bytes free]`: every task above 512 bytes (this is bytes on the ESP32).
- `heap_free` stays steady over 10 minutes (no steady decline).
- `[link] backpressure` and `lock_to` stay 0 or very small.

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
| Shake sends TAMPER (log) | yes | | |
| Peak deviation numbers (record) | (record) | | |
| Strike only when over 1500 mg | yes | | |
| Wi-Fi connects | yes | | |
| NTP time correct on IDLE screen | yes | | |
| MCX accepts TIME_SYNC | `time_sync=1/0` | | |
| Headless mode without the OLED | yes | | |
| Min stack bytes free | > 512 | | |
