# Defuse to Wake — CG2271 Assignment 4 Mini Project

## Purpose
Workspace for the CG2271 As4 mini project (15% of grade, 50 marks).
A smart lecture alarm: it reads the user's NUSMods timetable and, at wake time, arms a
"bomb" that only stops ringing once the user defuses it by clearing sensor-based modules
(safe dial, wire cutter, distance tripwire) without tampering. The SoCLaaS LLM writes the
clues and sets difficulty. MCXC444 (FreeRTOS) is the game engine; ESP32 is the renderer
and network gateway. Design rule: the alarm/bomb loop never depends on the internet —
everything online is fetched ahead of time, cached, and has a fallback.
Team-approved breakdown: https://claude.ai/code/artifact/64a40d7f-e2d9-43d7-9e73-d09a5ed75ee9

## Deadlines
- Interim report (5): 2 pages + code zip — Fri 23 Oct 2026, 2359 (B0X_YY_Interim.pdf, B0X_YY_Interim_Code.zip)
- Demo (35): Week 13 lab, 5–7 min, TAs inspect tasks/priorities/ISRs in code
- Final report (10): 5–10 pages + code zip — Fri 13 Nov 2026, 2359 (B0X_YY_Final.pdf, B0X_YY_Code.zip)
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
Stretch (only after core is done): laser tripwire, magnetic key (reed/Hall), clap code (mic).

## Software architecture (target)
- Game FSM (MCX): IDLE → ARMED → BOMB_ACTIVE (module 1..N) → DEFUSED | EXPLODED
  (EXPLODED → new bomb with +1 module). 3 strikes or timeout = EXPLODED.
- MCX tasks (high→low): game/alarm FSM + buzzer > joystick sampler > sensor task
  (ultrasonic) > comm RX parser > comm TX.
- MCX ISRs: UART RX (bytes → queue), PORT (encoder, buttons → queue/notification),
  TPM capture (echo). ISR priority: UART RX above GPIO.
- Sync: queues (ISR→parser, input events→FSM, FSM→TX), mutex on UART TX + shared game state.
- FreeRTOSConfig: configUSE_PREEMPTION=1, configUSE_TIME_SLICING=1, unused features disabled.
- ESP32 (Arduino-on-FreeRTOS, xTaskCreatePinnedToCore): network task core 0 (WiFiMulti,
  NTP, NUSMods, SoCLaaS, Firebase); UART task, display task, gyro task core 1; queues between.
- Inter-board protocol: UART 115200 8N1. Frame [0xAA][TYPE][SEQ][LEN][PAYLOAD 0–32][CRC-8],
  ACK/NACK, retransmit (timeout + max retries), 1 s heartbeat. shared/protocol.h + .c are
  canonical and compiled unchanged on both boards.
- Online (all cached, all with fallbacks): NUSMods public API (timetable cached in flash),
  SoCLaaS LLM (model x-test-1 → x-test-2 → built-in template clues; reply cached; called at
  start-up/nightly, never at alarm time), Firebase Realtime Database REST writes of results
  (failed writes queued and retried), NTP. Wi-Fi: WiFiMulti with 2 saved hotspots.
- The ESP32 picks puzzle answers deterministically; the LLM only writes clues and difficulty
  as JSON. Validate and clamp every field before use.
- Optional, last: small status page on the ESP32 built-in WebServer (live state, ring-now).
- Dropped on purpose (failure points, no extra marks): weather API, cloud dashboard,
  Wi-Fi setup portal/QR, async web server libraries, DS1302, SD card.

## Game rules (team-approved)
- Dial: 4-digit code; encoder changes current digit (wraps 0–9), press locks; wrong = strike, digit resets.
- Wires: 3–6 wires; joystick moves cursor, press cuts; wrong = strike, cut wire stays cut.
- Tripwire: hand in distance band continuously 3 s; leaving band resets hold, no strike.
- Tamper: shake over threshold = strike, 2 s cooldown.
- Max strikes or timer 0 = EXPLODED → after 3 s new bomb with +1 module (cap 4). All modules = DEFUSED.
- Snooze: encoder long-press or ESP32 CMD → +5 min, +1 module next bomb, max 2.
- Default offline bomb: dial + wires + tripwire, 180 s. All ESP32-sent config is range-checked in game/.

## Rubric map (demo, 35)
Sensors/actuators 5 · Poll+IRQ 2 · ISR design 3 · Task design 4 · Sync 2 · RTOS config 2 ·
ESP32 use 7 · Comms protocol 5 (packetization + flow control) · Online 5 (GenAI + REST + DB).
Every feature must name the rubric line it serves.

## In scope
MCX firmware, ESP32 firmware, shared protocol library, host-side test tools, optional ESP32
status page, report drafts and diagrams in docs/.

## Out of scope
Buying more hardware, camera/vision, mobile apps, cloud-hosted web apps, custom PCBs,
NUS_STU WPA2-Enterprise, the dropped items above, features that don't map to a rubric line.

## Stack & tools
- MCXC444: MCUXpresso IDE + MCUXpresso SDK (FreeRTOS), C.
- ESP32: Arduino IDE, ESP32 core, C++. Libraries: WiFi/WiFiMulti, WiFiClientSecure, HTTPClient,
  Preferences, ArduinoJson, Adafruit_SSD1306/GFX, Adafruit_MPU6050, WebServer (others need planner approval).

## Layout
- mcxc444/ — MCUXpresso project (created by the user in the IDE; workhorse edits its source/)
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
- Lab slot (B0X) and group number (YY) for file names.
- SoCLaaS API details (path, auth header, JSON mode) are behind a SoC login and need pasting in;
  access from a phone hotspot is untested (AWS relay 13.212.116.159 is the fallback).
- Ask TA whether GenAI + REST suffices or a DB operation is expected (we do a DB write anyway).
- OLEDs (2× SSD1306) ordered 15 Oct, awaiting delivery.
- ESP32 board model unconfirmed: spec sample output shows ESP32-S2 (single core) — affects
  core pinning, UART and I2C pins. Confirm before any ESP32 firmware prompt.
- MCUXpresso IDE install location unknown (not under C:\nxp on this machine).
