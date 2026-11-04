# Final report: TODO fixes

Two parts:
- **Part A:** replacement text you can paste into the report now. Every number in it was checked
  against the code on `master` (`1e394ba`) on 4 Nov 2026.
- **Part B:** one bench session (about 60 to 75 minutes, both boards) that produces every number the
  remaining TODOs need. Fill in the blanks (`___`) as you go, then paste the matching Part C text.

The wiring diagram for Figure 2 is ready: `docs/final/wiring.png` (source `wiring.svg`).

---

## Part A: paste now (no hardware needed)

### A1. Cover page: members 3 and 4
Only the team can fill this in. Git history shows commits by Ryan Koh, so he is probably one of the two.
Get both names and matric numbers from the members themselves; don't guess the matric numbers.

### A2. Figure: wiring photo or circuit diagram
Replace `[TODO: insert image wiring.png ...]` with `docs/final/wiring.png` and this caption:

> Figure 2. Wiring of both boards. Header pins from the FRDM-MCXC444 board manual; the HC-SR04 echo
> goes through a 1 kΩ / 2 kΩ divider and the KY-018 output has an external pull-up to 3.3 V.

If the bench values in B1 or B2 differ from 1 kΩ / 2 kΩ / 1 kΩ, ask to have the two labels in
`wiring.svg` changed and the PNG re-rendered (a one-minute job), and update the caption to match.

### A3. Task table, debug row: resolve the build-flag TODO
The current code builds the debug task: `APP_DEBUG_STATS 1` and `APP_GAME_DEBUG 1`, `APP_DRIVER_TEST 0`
(`app_config.h:16-22`). Replace the debug row's last cell with:

> stats every 10 s and one line per game-state change on the debug console (APP_DEBUG_STATS 1,
> APP_GAME_DEBUG 1 in the submitted build)

(Only if you turn those flags off before submitting, write instead: "not created in the submitted build".)

### A4. Lights rule: resolve the threshold TODO
If nobody changes the lights code before submission, the sentence is already exact (`game.c:356`,
`game_config.h:60-64`). Delete `[TODO: update when the final torch-only threshold is fixed]` and,
optionally, add:

> In practice a phone torch held about 5 cm away drives the level close to 255, above every possible
> target (at most 240).

If someone does change `GAME_LIGHT_DELTA`, `GAME_LIGHT_MIN_LEVEL` or `GAME_LIGHT_TARGET_CAP`, rewrite
the formula with the new values.

### A5. Host-test sentence: update the date
All 15 host test programs were rebuilt with `-Wall -Wextra -Werror` and pass on 4 Nov 2026. Replace
"all passing when run on 28 Oct 2026" with:

> all passing when last run on 4 Nov 2026 (about 2.8 million checks in total)

The per-program counts you quote (91, 148, 353, 209, 6237, 39573) are unchanged.

### A6. Add: the on-board limit (one sentence, Sensors and Actuators section)
After "The ESP32 BOOT button and the MCX switch SW3 trigger a demo ring after 3 s.", add:

> The RGB LED is our only on-board actuator and SW3 our only on-board input, within the brief's limit
> of one each; the board's own light sensor is not used (the LDR is the external KY-018 module).

### A7. Add: the offline default bomb (one sentence, State Management section)
After the config-validation paragraph, add:

> Until the ESP32 sends a BOMB_CONFIG, the MCX uses a built-in bomb (game_config.h): code 4-2-7-1,
> 4 wires with the third correct, tripwire band 15 to 25 cm, 180 s, 3 strikes, so the alarm works
> even if the ESP32 never comes up.

---

## Part B: bench session (both boards, `master` firmware)

Setup: flash both boards from `master`. Open the MCX debug console (MCU-Link VCOM, 115200), the ESP32
Serial monitor (115200), and `http://defuse.local/eng` on a laptop on the same hotspot.

| # | Measure | How | Result |
|---|---|---|---|
| B1 | Echo divider values | Read the two resistors on the breadboard (colour code or multimeter). Optional: measure the voltage at PTA13 while the echo is high (hand at 20 cm); it must be ≤ 3.3 V | series ___ Ω, to GND ___ Ω, PTA13 high = ___ V |
| B2 | LDR pull-up value | Read the resistor between the KY-018 signal pin and 3.3 V | ___ Ω |
| B3 | Ultrasonic edge rule (`APP_US_USE_PIN_LEVEL 1`) | `docs/hw_test_mcx_drivers.md` §3.4: hand at 10, 20 and 50 cm, then open space. Read `[evt] DISTANCE` lines | 10 cm → ___, 20 cm → ___, 50 cm → ___, open → 999? ___ |
| B4 | Tripwire hold | Ring a bomb (BOOT), reach the tripwire, hold a hand in the band shown on the OLED | clears after ~3 s? ___ ; a quick wobble out of the band forgiven? ___ |
| B5 | Lights | `docs/hw_test_esp32_basic.md` §3.2b, torch test | clears after ~2 s with torch? ___ ; room light alone? ___ |
| B6 | Retransmit / NACK | On `/eng` note the link tiles (tx, rx, retx, crc, nack). Press **Corrupt next packet**, then **Ring now**. Repeat 5 times. Note the tiles again, and the MCX `[link] ... crc=.. nack=..` line | before: retx ___ crc ___ nack ___ ; after: retx ___ crc ___ nack ___ ; every bomb started correctly? ___ |
| B7 | LLM latency and verified share | ESP32 Serial: type `puzzles`, wait for `new set stored`, then `ai`. Each puzzle line shows `setter ms / checker ms` and `AI verified` or `bank` (`docs/hw_test_esp32_ai.md` §2.1) | ___ of 8 AI-verified; setter ___ to ___ ms; checker ___ to ___ ms; run took ___ min |
| B8 | End-to-end | Play 5 full bombs (BOOT or web Ring now): dial, wires, tripwire, lights. In 2 of them make one wrong dial digit on purpose; let 1 time out | 5 rings: ___ defused, ___ exploded as intended; any unexpected fault: ___ ; `hist` shows 5 records? ___ ; Firebase pending drains to 0? ___ |
| B9 | Offline | `/eng` **Simulate internet outage**, then Ring now and defuse | bomb works offline? ___ |

Take a photo of the wired setup during the session too (optional second figure).

---

## Part C: text to paste once Part B is done

### C1. Ultrasonic row (Sensors table) and the "Noisy tripwire" challenge
If B3 passed (readings within ±2 cm, open space 999), replace
"implemented but not yet verified on hardware [TODO: confirm on hardware]" with:

> verified on the bench (10, 20 and 50 cm read ___, ___ and ___ cm; open space reads 999)

and in Challenges replace "(implemented; [TODO: confirm on hardware])" with "(verified on the bench)".

If B3 failed and you switched to `APP_US_USE_PIN_LEVEL 0`, write instead:

> the pin-level check did not work reliably on our board, so the submitted build classifies edges by
> their order only (APP_US_USE_PIN_LEVEL 0); readings were within ±2 cm at 10 to 50 cm

### C2. Wiring rules: divider and pull-up
Replace "(our pin table proposes 1 kΩ in series and 2 kΩ to ground) [TODO: confirm the divider values used]" with:

> (___ Ω in series and ___ Ω to ground, so the pin sees about ___ V)

In Challenges, replace "1 kΩ pull-up ... [TODO: confirm the resistor value on the bench]" with the B2 value.
Then add the pull-up to `docs/pin_table.md` (the report says it is missing there).

### C3. Communication Protocol: measured counts
Replace `[TODO: add measured retransmit and NACK counts from a bench run]` with:

> In a bench run we corrupted 5 frames with the fault button: the MCX counted 5 CRC errors and sent
> 5 NACKs, the ESP32 made 5 retransmissions, and every bomb still started with the right
> configuration (retx ___ → ___, nack ___ → ___ on the engineering page).

(Use your real numbers; if a count differs from 5, say why, e.g. an extra retransmit after a timeout.)

### C4. Online Function: latencies and verified share
Replace `[TODO: add measured latencies and the share of AI-verified puzzles]` with:

> In a bench run ___ of 8 puzzles were AI-verified (the rest came from the bank); setter calls took
> ___ to ___ s and checker calls ___ to ___ s, and the whole run took about ___ minutes, all while the
> bomb stayed playable.

### C5. Additional Features and Conclusion: end-to-end results
Replace both `[TODO: end-to-end ...]` with:

> On the bench we ran ___ complete four-module bombs on both boards: ___ defused, ___ exploded as
> intended (wrong digits and a timeout), with no unexpected fault; each defuse added a history record
> and the Firebase queue drained to 0. With the simulated internet outage the bomb still rang and
> defused.

In the Conclusion's "Next steps", delete "record retransmit and latency figures" and "run the full
four-module bomb repeatedly on both boards" once C3 to C5 are in.

---

## Last checks before exporting the PDF
- Search the document for `TODO`, `[`, `___`: none should remain.
- Page count 5 to 10 at 12 pt, 1.15 spacing, justified. If it's over 10, cut first: the requirements
  checklist table, then the ESP32 task table (fold into two sentences), then the ISR table.
- Save as `B02_10_Final.pdf`. Build the code zip with `tools/make_submission.ps1 -Kind Final`
  (→ `B02_10_Code.zip`) and check it contains no `secrets.h` or API key.
