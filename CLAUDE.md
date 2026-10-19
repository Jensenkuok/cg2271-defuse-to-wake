# Defuse to Wake — CG2271 Assignment 4 Mini Project

## Purpose
Workspace for the CG2271 As4 mini project (15% of grade, 50 marks).
A smart lecture alarm: it reads the user's NUSMods timetable and, at wake time, arms a
"bomb" that only stops ringing once the user defuses it by clearing sensor-based modules
(safe dial, wire cutter, distance tripwire) without tampering. The SoCLaaS LLM writes
verified brain-teasers for the dial and wires (to wake the brain up) and, after defusing, a
morning briefing + coach/roast message from today's timetable and the user's history. MCXC444 (FreeRTOS) is the game engine; ESP32 is the renderer
and network gateway. Design rule: the alarm/bomb loop never depends on the internet —
everything online is fetched ahead of time, cached, and has a fallback.
Team-approved breakdown: https://claude.ai/code/artifact/64a40d7f-e2d9-43d7-9e73-d09a5ed75ee9

## Deadlines
- Team: lab slot B02, group 10 → file names B02_10_*.
- Interim report (5): 2 pages + code zip — Fri 23 Oct 2026, 2359 (B02_10_Interim.pdf, B02_10_Interim_Code.zip)
- Demo (35): Week 13 lab, 5–7 min, TAs inspect tasks/priorities/ISRs in code
- Final report (10): 5–10 pages + code zip — Fri 13 Nov 2026, 2359 (B02_10_Final.pdf, B02_10_Code.zip)
- Report format: standard font, 12pt, 1.15 spacing, justified.

## Workflow
A planner session writes prompts; a workhorse session executes them. The workhorse never asks
the user questions: on ambiguity, blockers or destructive actions it stops that part and reports.
The workhorse cannot see hardware: the user flashes, tests and reports results.

## Hardware (group of 4–5: 4 sensors, 3 actuators; pins TBD from board docs, never guessed)
Sensors
| Part (kit) | Board | Mode | Module / purpose |
|---|---|---|---|
| Rotary encoder (+ push) | MCX GPIO | Interrupt (A/B edges, push) | Safe dial: enter code digits |
| PS2 joystick (+ button) | MCX ADC | Polled 50 Hz (vTaskDelayUntil); button IRQ | Wire cutter: move cursor, cut wire |
| HC-SR04 ultrasonic | MCX | Periodic trigger, echo via TPM input capture | Distance tripwire: hold hand in band for 3 s |
| GY-521 MPU-6050 gyro (I2C) | ESP32 | Polled by ESP32 task, library | Anti-tamper: shake/tilt = strike (event sent to MCX) |
Fallback: if the shared I2C bus misbehaves, the kit's shock switch replaces the gyro on an MCX interrupt.
Actuators
| Part | Board | Purpose |
|---|---|---|
| Passive buzzer | MCX TPM PWM | Ticking that speeds up, explosion, defuse jingle |
| RGB LED (on-board) | MCX TPM PWM | Strike/progress/status colours |
| SSD1306 0.96" OLED (I2C 0x3C, 2 bought) | ESP32 | Bomb screen: timer, module graphics, clues, briefing |
Wiring rules: HC-SR04 echo (5 V) through a voltage divider to the MCX pin; joystick and encoder
on 3.3 V; each board on its own USB, grounds tied together.
Proven pins (from the team's Lab 2/3 code — reuse, don't change):
- Board link: MCX UART2 TX PTE22 / RX PTE23 (PORT mux 4) ↔ ESP32-S2 Serial1 RX GPIO2 / TX GPIO1.
- On-board RGB LED via TPM0 PWM: red PTE31 (TPM0_CH4, mux 3), green PTD5 (TPM0_CH5, mux 4),
  blue PTE29 (TPM0_CH2). TPM clock source MCGIRCLK (SIM->SOPT2 TPMSRC = 3).
- Joystick X/Y: PTE20 (ADC0_SE0), PTE21 (ADC0_SE4a). On-board switch SW3: PTA4.
All other pins are chosen and justified in docs/pin_table.md (verified against the RM and UM12120).
Stretch (only after core is done): laser tripwire, magnetic key (reed/Hall), clap code (mic).

## Software architecture (target)
- Game FSM (MCX): IDLE → ARMED → BOMB_ACTIVE (module 1..N) → DEFUSED | EXPLODED
  (EXPLODED → new bomb with +1 module). 3 strikes or timeout = EXPLODED.
- MCX task priorities (configMAX_PRIORITIES 5): game 4 > joystick sampler 3 > comm (owns
  proto_link) 2 = ultrasonic 2 (equal on purpose: shows time slicing) > feedback (buzzer + LED)
  1 = debug/stats 1 > idle 0.
- MCX ISRs: UART RX (bytes → queue), PORT (encoder, buttons → queue/notification),
  TPM capture (echo). ISR priority: UART RX above GPIO.
- Sync: queues (ISR→parser, input events→FSM, FSM→TX), mutex on UART TX + shared game state.
- FreeRTOSConfig: configUSE_PREEMPTION=1, configUSE_TIME_SLICING=1, unused features disabled.
- ESP32-S2 is SINGLE-CORE (Arduino-on-FreeRTOS, xTaskCreate with priorities, no core pinning):
  UART link task > gyro task > display task > network task (WiFiMulti, NTP, NUSMods, SoCLaaS,
  Firebase) = web page task (both lowest: slow and latency-tolerant); queues between.
- ESP32 rule: always send TIME_SYNC before SET_ALARM, and re-send SET_ALARM after every TIME_SYNC
  (the MCX does not move an ARMED deadline when the clock changes).
- Alarm = the FIRST class of each day only (start − lead time). After DEFUSED, or once today's
  first class has started, schedule the next day's first class.
- Session guard: from the first BOMB_ACTIVE until DEFUSED → IDLE, the ESP32 sends NO SET_ALARM
  and NO CMD_CANCEL (a snooze leaves the MCX ARMED; resending would overwrite or kill it).
  Outside a session, a cleared alarm is sent as CMD_CANCEL.
- Known limitation: no battery-backed RTC; after a full power-up the device needs Wi-Fi once for
  NTP before it can schedule. The MCX keeps an armed alarm across ESP32-only resets.
- Inter-board protocol: UART 115200 8N1. Frame [0xAA][TYPE][SEQ][LEN][PAYLOAD 0–32][CRC-8],
  ACK/NACK, retransmit (timeout + max retries), 1 s heartbeat. shared/protocol.h + .c are
  canonical and compiled unchanged on both boards.
- Online (all cached, all with fallbacks): NUSMods public API (timetable cached in flash),
  SoCLaaS LLM (model x-test-1 → x-test-2 → built-in puzzle bank / template briefing; everything
  generated nightly and cached in flash, never at alarm time), results stored locally in ESP32
  flash (last 14 mornings; source of truth for the coach and difficulty), Firebase Realtime
  Database as a write-only mirror (queued and retried, never read at runtime), NTP. Wi-Fi: WiFiMulti with 2 saved hotspots.
- NUSMods facts: AY2026/27 Sem 1 (api.nusmods.com/v2/2026-2027/modules/<CODE>.json, ~5 KB each).
  Week 1 starts Mon 10 Aug 2026; recess week 21–27 Sep (after week 6); derived from the spec's
  dates (Week 10 Fri = 23 Oct, Week 13 Fri = 13 Nov). Share link format:
  ...?CG2271=LAB:02,LEC:01,TUT:02&QF2104=LEC:1,TUT:4 (classNo padding varies; one slot may have
  several sessions; lesson weeks can be partial, e.g. [9]).
- Puzzle pipeline (per puzzle): (1) the ESP32 picks the answer (4-digit dial code, or wire
  index < wire_count); (2) setter call: the LLM writes a ≤ 80-character puzzle with that unique
  answer; (3) checker call: a separate request with only the puzzle text (no answer) returns the
  LLM's answer as JSON; (4) keep the puzzle only on an exact match, up to 3 attempts, else the
  built-in bank. The nightly run caches 4 dial + 4 wire puzzles (snooze/explosion bombs use the
  next ones). Validate and clamp every field; the answer always comes from the ESP32, never the LLM.
- Cross-model verification: setter = x-test-2 (currently DeepSeek V4.1 Flash 1017), checker =
  x-test-1 (currently GLM 5.3 Flash); if one model is unavailable, the other does both jobs;
  if both fail, use stable qwen3.6:35b, then "default" (all four verified working 19 Oct 2026).
  Model IDs are configurable (SoCLaaS swaps models behind IDs without notice). Parse only the
  JSON answer from message.content (reasoning models may add extra text); allow enough max_tokens.
- Local web page (CORE), served by the ESP32 built-in WebServer on the same Wi-Fi, in its own
  lowest-priority task (the game never depends on it). Tabs:
  - "My mornings": next alarm, today's lectures, last 14 results + streak, settings form
    (NUSMods share link, wake-up lead time before first lecture), "Ring now". Settings are saved
    in ESP32 flash (Preferences); secrets.h keeps only Wi-Fi credentials and API keys.
  - "Engineering" (demo): live bomb state, link stats + log of the last 20 frames
    (type/seq/ACK/NACK/retransmit), AI panel (tonight's puzzles, setter model, checker answer,
    verified/rejected), Firebase mirror queue, fault buttons: "Corrupt next packet" (bad CRC on
    the next ESP32->MCX frame), "Simulate Wi-Fi down".
  - Plain HTML/JS served from flash, JSON status endpoint polled about every 500 ms; no
    external CDNs or async web server libraries.
- STRETCH (Week 12, only after integration passes): online monitor = static page on Firebase
  Hosting reading a live-state path the ESP32 mirrors; remote settings written there are pulled
  by the ESP32 only at its nightly sync (last local settings win on failure); a "hint" button
  (one per bomb, costs a strike). The alarm and game must never wait on it.
- Dropped on purpose (failure points, no extra marks): weather API, cloud dashboard (except the
  stretch monitor above), laptop-tethered networking, Wi-Fi setup portal/QR, async web server
  libraries, DS1302, SD card.

## Game rules (team-approved)
- Dial: 4-digit code; encoder changes current digit (wraps 0–9), press locks; wrong = strike, digit resets.
- Wires: 3–6 wires; joystick moves cursor, press cuts; wrong = strike, cut wire stays cut.
- Tripwire: hand in distance band continuously 3 s; leaving band resets hold, no strike.
- Tamper: shake over threshold = strike, 2 s cooldown.
- Max strikes or timer 0 = EXPLODED → after 3 s new bomb with +1 module (cap 4). All modules = DEFUSED.
- Snooze: encoder long-press or ESP32 CMD → +5 min, +1 module next bomb, max 2.
- Default offline bomb: dial + wires + tripwire, 180 s. All ESP32-sent config is range-checked in game/.
- Puzzles: dial and wire answers are the ESP32's targets, shown as verified brain-teasers on the
  OLED; one strike reveals a plain hint (e.g. "code = 4 2 _ _").
- Difficulty (module count, time limit) is a deterministic rule from recent snoozes/strikes, not
  the LLM; the coach message explains it.

## Rubric map (demo, 35)
Sensors/actuators 5 · Poll+IRQ 2 · ISR design 3 · Task design 4 · Sync 2 · RTOS config 2 ·
ESP32 use 7 · Comms protocol 5 (packetization + flow control) · Online 5 (GenAI + REST + DB).
Every feature must name the rubric line it serves.

## In scope
MCX firmware, ESP32 firmware, shared protocol library, host-side test tools, ESP32-served local
web page, report drafts and diagrams in docs/; the online monitor only as the Week-12 stretch.

## Out of scope
Buying more hardware, camera/vision, mobile apps, cloud-hosted web apps (except the stretch
monitor), PC-side software the product depends on, custom PCBs,
NUS_STU WPA2-Enterprise, the dropped items above, features that don't map to a rubric line.

## Stack & tools
- MCXC444: MCUXpresso IDE 25.6.136 at D:\D\MCUXpressoIDE_25.6.136 (headless build:
  ide\mcuxpressoidec.exe), SDK_2.x_FRDM-MCXC444 26.6.0 installed, FreeRTOS, C. Cortex-M0+
  (2 NVIC priority bits: 0/64/128/192; the CM0 port masks interrupts with PRIMASK).
- MCX peripheral code is register-level (SIM, PORT, UART, TPM, ADC, NVIC), matching the labs;
  fsl drivers only where the template project already uses them for board init/debug console.
- References: RM D:\NUS\Year 3\Y3S1\CG2271\Datasheet\MCXC44XP64M48RM.pdf, board manual
  D:\NUS\Year 3\Y3S1\CG2271\Datasheet\UM12120.pdf, lab code under D:\NUS\Year 3\Y3S1\CG2271\Labs
  and C:\Users\rkoh2\Documents\MCUXpressoIDE_25.6.136\workspace (2271UART = Lab 3 FreeRTOS project).
- ESP32: ESP32-S2 board, FQBN esp32:esp32:esp32s2, ESP32 core 3.3.12. CLI compile with the
  arduino-cli bundled in Arduino IDE: C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe. C++. Libraries: WiFi/WiFiMulti, WiFiClientSecure, HTTPClient,
  Preferences, ArduinoJson, Adafruit_SSD1306/GFX, Adafruit_MPU6050, WebServer (others need planner approval).
- SoCLaaS: OpenAI-compatible, POST https://soclaas-api.comp.nus.edu.sg/v1/chat/completions with
  header "Authorization: Bearer <key>". The API is on a public IP; the key portal, docs and Open
  WebUI work on the NUS network only.

## Layout
- mcxc444/defuse_mcx/ — MCUXpresso FreeRTOS project (cloned from the Lab 3 project);
  source/shared and source/game are synced copies of shared/ and game/ (tools/sync_shared.ps1)
- esp32/defuse_esp32/ — Arduino sketch (secrets.h git-ignored; secrets.example.h committed)
- shared/ — protocol + proto_link (ACK/retransmit), canonical for both boards
- game/ — hardware-independent bomb game engine (pure C99, host-tested), compiled into the
  MCX project like shared/; MCX tasks feed it events and act on its outputs
- tools/ — host-side tests/scripts
- docs/ — interim/final report drafts, diagrams, pin table

## Conventions
- ISRs: only ...FromISR + portYIELD_FROM_ISR; no printf, delays, loops waiting on input;
  NVIC priority numerically ≥ configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
- Tasks block on queues/notifications or vTaskDelayUntil; no busy-waiting.
- No magic numbers: constants in config headers. One module per peripheral. No dynamic
  allocation after start-up.
- Comments explain RTOS choices (priority, why polled vs IRQ): TAs read the code at the demo.
- Never commit secrets; scrub keys before building submission zips.
- Git with conventional commits (feat/fix/docs/test/chore); no AI attribution lines.

## Definition of done (per feature)
Builds with zero warnings in its IDE; host-testable logic (protocol, CRC, FSM) has passing
tests; hardware test steps written for the user; rubric line noted; user confirms on hardware.

## Open items
- SoCLaaS key created and working (base URL https://soclaas-api.comp.nus.edu.sg/v1, 17 models);
  confirmed reachable from a phone hotspot (19 Oct 2026), so no relay is needed.
- Ask TA whether GenAI + REST suffices or a DB operation is expected (we do a DB write anyway).
- OLEDs (2× SSD1306) ordered 15 Oct, awaiting delivery.
- ESP32-S2 I2C pins for OLED + gyro not yet chosen (check the board's header silkscreen).
