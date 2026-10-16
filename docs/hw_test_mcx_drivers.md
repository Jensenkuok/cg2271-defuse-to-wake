# Hardware test: MCXC444 input and feedback drivers

Goal: check every sensor and actuator driver on the real board before the game task is added.
In driver-test mode the debug task prints every input event and runs a sound + LED demo at boot.
Rubric lines: Sensors/actuators, Poll+IRQ, ISR design, Task design.

## 1. Wiring (from docs/pin_table.md)

Power the parts from the FRDM header J3: **P3V3 = J3[8]**, **P5V0 = J3[10]**, **GND = J3[4] /
J3[12] / J3[14]** (or J2[14]).

| Part | Part pin | FRDM pin | MCU pin | Notes |
|---|---|---|---|---|
| Rotary encoder | A (CLK) | J1[8] | PTA12 | encoder powered from **3.3 V** |
| | B (DT) | J1[12] | PTA5 | |
| | SW | J4[12] | PTC1 | internal pull-up; switch to GND |
| | + / GND | J3[8] / GND | – | **3.3 V**, never 5 V |
| PS2 joystick | VRx | J4[1] | PTE20 (ADC0_SE0) | joystick powered from **3.3 V** |
| | VRy | J4[3] | PTE21 (ADC0_SE4a) | |
| | SW | J4[10] | PTC2 | internal pull-up |
| | +5V pin / GND | J3[8] (3.3 V!) / GND | – | feed the "+5V" pin with **3.3 V** so VRx/VRy stay ≤ 3.3 V |
| HC-SR04 | VCC / GND | J3[10] (5 V) / GND | – | |
| | TRIG | J4[8] | PTB3 | 3.3 V drive (UNVERIFIED for the HC-SR04, see pin table) |
| | ECHO | J2[2] | PTA13 (TPM1_CH1) | **5 V output: use a divider.** ECHO → 1 kΩ → PTA13, and PTA13 → 2 kΩ → GND (≈ 3.3 V) |
| Passive buzzer | + / signal | J4[6] | PTB2 (TPM2_CH0) | other leg to GND |
| RGB LED | – | on-board | PTE31 / PTD5 / PTE29 | nothing to wire |
| SW3 | – | on-board | PTA4 | nothing to wire |

Do not connect anything to PTE22/PTE23 (UART2) for this test.

## 2. Enable driver-test mode

1. In `mcxc444/defuse_mcx/source/app_config.h` set

   ```c
   #define APP_DRIVER_TEST             1
   ```

2. Build (expect `0 errors, 0 warnings`), flash and Resume (F8).
3. Open the MCU-Link VCOM port at 115200 8N1. Start-up prints
   `Defuse to Wake MCX [DRIVER TEST]`.
4. The first 14 s are the boot demo (section 3.6). After that the console prints
   `[demo] done, printing input events`, then one `[evt] ...` line per input event and an
   `[io] ...` + `[link] ...` + `[stack words free] ...` block every 5 s.

Note: `[evt] DISTANCE ...` appears about 16 times a second (one per 60 ms ping). That is by
design; look past it for the other events.

## 3. Checklists

### 3.1 Encoder (rotation)
- Turn one detent clockwise. Expect exactly one `[evt] ENC_STEP 1`.
- Turn one detent anticlockwise. Expect exactly one `[evt] ENC_STEP -1`.
- Turn 10 detents quickly each way. The `enc=` value in the next `[io]` line changes by +10, then −10.
- PASS: one event per detent, correct sign, no extra events when the knob is left alone.
  If the sign is reversed, set `APP_ENC_INVERT 1`. If you get one event per 2 or 4 detents,
  change `APP_ENC_TRANSITIONS_PER_STEP` (encoder-model dependent).

### 3.2 Encoder button (short / long)
- Short press (< 0.8 s). Expect `[evt] ENC_PRESS 0` on **release**.
- Long press (hold ≥ 1 s). Expect `[evt] ENC_LONG_PRESS 0` on release.
- PASS: exactly one event per press, and the right type 10 times out of 10.

### 3.3 Joystick
- Leave it centred. The `[io]` line shows `joy_x` and `joy_y` around 2048 (roughly 1700–2400).
- Push left. Expect one `[evt] JOY_LEFT <x>` (x < 1000). Hold it: no repeats.
  Return to the centre, push left again: one more event.
- Push right. Expect one `[evt] JOY_RIGHT <x>` (x > 3000).
- Press the stick. Expect `[evt] JOY_PRESS 0` on press.
- PASS: one event per push, LEFT/RIGHT match the physical direction (otherwise set
  `APP_JOY_X_INVERT 1`), `adc_err=0` in `[io]`.

### 3.4 Ultrasonic
- Hold a flat object (a book) at **10 cm**, **20 cm** and **50 cm** from the sensor face.
- Expect `[evt] DISTANCE` values around 10, 20 and 50 (±2 cm). `dist_cm=` in `[io]` agrees.
- Point it at open space (> 4 m) or cover nothing. Expect `DISTANCE 999`.
- PASS: within ±2 cm at all three distances. `us_timeouts` grows only for the open-space test.

### 3.5 SW3 (bench trigger)
- Press SW3 (on-board, labelled ISP). Expect `[evt] CMD_RING_NOW 3`.
- PASS: one event per press.

### 3.6 Buzzer sounds and LED modes (boot demo)
After each reset the console prints one line per 2 s step. Check that each sound is distinct
and each LED mode matches:

| Step | Sound (heard once) | LED mode (2 s) | Expected LED |
|---|---|---|---|
| 1 | CLICK: very short high tick | OFF | dark; a short blue blink every 2 s (no ESP32, so the link is down) |
| 2 | DIGIT_OK: two rising beeps | ARMED_DIM_BLUE | dim steady blue |
| 3 | STRIKE: low buzz | ACTIVE_RED_BLINK | red blinking about twice a second |
| 4 | MODULE_CLEARED: three rising beeps | DEFUSED_GREEN | solid green |
| 5 | DEFUSED: four-note rising jingle | EXPLODED_RED_WHITE | fast red/white alternation |
| 6 | EXPLODED: three falling low notes | – | (stays EXPLODED) |
| 7 | DENIED: two short mid beeps | – | then back to OFF |

- PASS: 7 distinct sounds, 5 correct LED modes, RED never stuck solid (solid red = fatal halt:
  stack overflow or heap exhausted).

### 3.7 Stack and queue health (from the 5 s block)
- `[stack words free]`: every value > 20 words.
- `drops=0` as long as the console keeps up.
- `heap_free=` is printed; note it in the results table.

## 4. Results (fill in)

| Check | Expected | Result | Notes |
|---|---|---|---|
| Encoder CW / CCW | ±1 per detent | | |
| Encoder short press | ENC_PRESS | | |
| Encoder long press | ENC_LONG_PRESS | | |
| Joystick left | JOY_LEFT once | | |
| Joystick right | JOY_RIGHT once | | |
| Joystick press | JOY_PRESS | | |
| Ultrasonic 10 cm | 8–12 | | |
| Ultrasonic 20 cm | 18–22 | | |
| Ultrasonic 50 cm | 48–52 | | |
| Ultrasonic open space | 999 | | |
| SW3 | CMD_RING_NOW 3 | | |
| 7 sounds distinct | yes | | |
| 5 LED modes correct | yes | | |
| Min stack words free | > 20 | | |
| heap_free | (record) | | |

## 5. Switch back
Set `APP_DRIVER_TEST` back to `0`, then rebuild and flash. In normal mode, nothing consumes the
input queue yet (the game task comes next). The debug task prints link and stack stats every
10 s; set `APP_DEBUG_STATS 0` to silence it.
