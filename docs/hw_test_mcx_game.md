# Hardware test: MCXC444 game on the bench (no ESP32)

Goal: play one full bomb on the MCX alone. The debug console stands in for the ESP32 OLED: it
prints one `[game]` line every time the game snapshot changes. SW3 stands in for the ESP32's
"ring now" command.
Rubric lines: Task design (game task prio 4), Sync (input queue, control queue, mutex-guarded
snapshot), Poll+IRQ (all inputs), Sensors/actuators.

Prerequisite: `docs/hw_test_mcx_drivers.md` passed (encoder, joystick, ultrasonic, buzzer and LED
work). Wiring is the same as in that guide. Nothing is connected to PTE22/PTE23.

## 1. Flash and open the console

1. Check `source/app_config.h`: `APP_DRIVER_TEST 0`, `APP_LOOPBACK_TEST 0`, `APP_GAME_DEBUG 1`.
2. Build (expect `0 errors, 0 warnings`), flash, Resume (F8).
3. Open the MCU-Link VCOM port at 115200 8N1. Expect `Defuse to Wake MCX` then
   `[game] IDLE`. The LED is dark with a short blue blink every 2 s (no ESP32, so the link is
   down; that is normal here).

## 2. Arm the bomb

Press **SW3**. Expect within 3 s:

```
[game] ARMED
[game] BOMB_ACTIVE DIAL mod 1/3 t=180s strikes=0 prog=0% dial digit=0 val=0
```

- LED: **dim steady blue** while ARMED, then **red blinking** (about 2 Hz) once active.
- Buzzer: a **tick once per second**, speeding up in the last 10 s.
- `[game] ... t=179s`, `t=178s`, … appear once a second.

## 3. Defuse the three default modules

Dial code **4-2-7-1**, wire **index 2** (the third wire, counting from 0), tripwire band
**18–22 cm** for 3 s.

| Step | Do this | Expected console line (examples) | Buzzer / LED |
|---|---|---|---|
| Dial, digit 1 | turn the encoder to **4**, press the encoder | `dial digit=0 val=4`, then `dial digit=1 val=0` | click per detent, two rising beeps (DIGIT_OK) |
| Dial, digits 2–4 | **2**, press; **7**, press; **1**, press | `dial digit=2 val=7`, `digit=3 val=1` | DIGIT_OK each; after the 4th press three rising beeps (MODULE_CLEARED) |
| Wires | joystick right twice, then press the stick | `mod 2/3 ... wires cursor=2 cut=0b000000`, then `mod 3/3` | click per move; MODULE_CLEARED |
| Tripwire | hold a flat hand 18–22 cm from the HC-SR04 for 3 s | `trip dist=20cm hold=0%` … `hold=50%` … `hold=100%`, then `[game] DEFUSED mod 3/3 ... prog=100%` | four-note rising jingle, **solid green** LED |

After DEFUSED the console shows nothing new, and **30 s later** `[game] IDLE`. The LED goes dark
(blue blink returns).

## 4. Failure paths

1. **Wrong digit = strike.** In a fresh bomb (press SW3 again), press the encoder with the wrong
   digit showing. Expect `strikes=1`, the digit resets to `val=0`, and a low STRIKE buzz.
2. **Three strikes = explosion.** Two more wrong presses. The third gives
   `[game] EXPLODED ...`, the falling EXPLODED sound and a **fast red/white** LED. Inputs are ignored.
   **3 s later** `[game] BOMB_ACTIVE DIAL mod 1/4 t=180s strikes=0 ...`: a new bomb with
   **one more module** (4 instead of 3; the 4th is a dial again).
3. **Wrong wire.** Cut any wire other than index 2: strike, the wire stays cut
   (`cut=0b000001` for wire 0), and pressing it again gives a DENIED double beep, no new strike.
4. **Snooze.** During a bomb, long-press the encoder (hold about 1 s). Expect `[game] ARMED`
   (dim blue). **5 minutes later** the bomb starts again with one more module. A second snooze
   works the same way. A third long press during a bomb plays DENIED and nothing changes.
   `APP_GAME_TICK_MS` is unchanged (50 ms): snooze only changes the game state, not the
   task timing. Use `CMD`/SW3 to re-ring sooner if you do not want to wait.
5. **Timeout.** Leave a bomb alone for 180 s: it explodes the same way.

## 5. Health check

Every 10 s the console prints the `[link] [comm] [drops] [ctrl] [stack words free]` block.
Expect:
- `[drops]` all 0.
- `[stack words free]`: every task above 20 words (note `game=`).
- `heap_free=` (record it; expect about 5000 bytes).
- `tick_overruns=0`, `us_stale=0` (small numbers are OK if you moved your hand during pings).

## 6. Results (fill in)

| Check | Expected | Result | Notes |
|---|---|---|---|
| SW3 arms in 3 s | `ARMED` then `BOMB_ACTIVE` | | |
| Dim blue while ARMED | yes | | |
| Red blink while active | yes | | |
| Timer tick 1 Hz, faster in last 10 s | yes | | |
| Dial 4-2-7-1 clears module 1 | `mod 2/3` | | |
| Wire 2 clears module 2 | `mod 3/3` | | |
| Tripwire hold 3 s | `DEFUSED` | | |
| Defused jingle + green LED | yes | | |
| IDLE after 30 s | yes | | |
| Wrong digit = strike | `strikes=1` | | |
| 3 strikes = EXPLODED | yes | | |
| New bomb 3 s later with 4 modules | `mod 1/4` | | |
| Wrong wire stays cut, re-press DENIED | yes | | |
| Snooze: ARMED, re-rings 5 min later | yes | | |
| 3rd snooze DENIED | yes | | |
| `[drops]` all 0 | yes | | |
| Min stack words free | > 20 | | |
| heap_free | (record) | | |
