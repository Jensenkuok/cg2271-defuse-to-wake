# Q&A by rubric line: where each requirement lives in the code

For the `qna-notes` branch: Ryan's latest code (`2d5e7a5`, 6 Nov) plus a few explanatory **comments only**. The
firmware it builds is the same as Ryan's `master`, so what you show on your laptop is what runs on
the demo board, as long as that board was flashed from Ryan's latest.

Line numbers point at the function (or setting) to show; the comment explaining it is directly above.
In MCUXpresso press Ctrl+L to jump to a line number.

Who answers follows `docs/demo_script.md` (A pitch + plays, B live bomb, C protocol + online,
D code walk). Everyone should still be able to answer the first line of every row.

## Getting it on your laptop
```
git fetch origin
git checkout qna-notes
```
- **MCUXpresso:** File > Import > Existing Projects into Workspace > `<repo>/mcxc444/defuse_mcx`,
  untick "Copy projects into workspace" (`mcxc444/README.md`). Paths below for the MCX are relative to
  that project (`source/...`).
- **Jump to a line:** open the file, press Ctrl+L, type the number.
- **ESP32 files** are under `esp32/defuse_esp32/src/app/`. Arduino IDE only shows the `.ino` as a tab,
  so open these in MCUXpresso (File > Open File) or VS Code.
- **Do not** click "Update Code" in MCUXpresso Config Tools: it regenerates the FreeRTOS config.

---

## Sufficient sensors/actuators (5) — B, A plays
**Need:** 4 sensors, 3 actuators; 2 marks for justifying the board, polled vs interrupt, priorities.
- **Sensors (all on the MCX):** rotary encoder (safe dial), PS2 joystick (wires), HC-SR04 (tripwire),
  KY-018 LDR (lights). **Actuators:** buzzer and RGB LED (MCX), SSD1306 OLED (ESP32). Gyro is off.
- **Why the MCX:** real-time inputs need interrupts, timer capture and PWM, and the bomb must work
  with no Wi-Fi. **Why the OLED on the ESP32:** 1 KB framebuffer + Adafruit libraries + RAM.

| Show | Where |
|---|---|
| Pins of every part | `docs/pin_table.md`, `source/app_config.h` (pins section) |
| Buzzer + LED PWM | `source/feedback.c:78` |
| OLED task | `esp32/.../display_task.cpp:700` |
| Wiring diagram / system diagram | `docs/final/wiring.png`, `docs/final/architecture.png` |

## Polling & Interrupt used (2) — B
**Need:** at least 1 polled sensor, at least 1 interrupt sensor.
- **Polled:** joystick X/Y every 20 ms and LDR every 100 ms, `vTaskDelayUntil`: `source/joystick.c:182`.
- **Interrupt:** encoder A/B edges `source/encoder.c:77`; encoder + joystick buttons
  `source/buttons.c:78`; ultrasonic echo by TPM1 input capture `source/ultrasonic.c:98`.
- **One-line reason:** analog voltages have no event to interrupt on, so poll them; encoder edges are
  fast and would be missed by polling.

## ISR Design (3) — D
**Need:** ISRs don't block, don't wait for input (e.g. UART), use sync tools correctly.
- Every ISR: read and clear hardware, hand off with `...FromISR`, end with `portYIELD_FROM_ISR`.
  No printf, no delay, no waiting loop.

| Show | Where |
|---|---|
| UART ISR: one byte into a queue, never waits for input | `source/uart_link.c:109` |
| Debounce without waiting (tick count) | `source/buttons.c:61` |
| ADC ISR wakes the joystick task (notification) | `source/joystick.c:79` |
| Interrupt priorities + why FromISR is legal at 64 | `source/app_config.h:71` |

## Task Design (4) — D
**Need:** at least 3 tasks, they yield, no obvious bugs.
- **MCX tasks:** game 4, joystick 3, comm 2, ultrasonic 2, feedback 1 (debug task off in this build).
  **ESP32 tasks:** link 4, display 2, network 1, web 1.
- **Yielding:** every task blocks on a queue, a notification or `vTaskDelayUntil`. The only busy wait is
  the 10 us ultrasonic trigger pulse.

| Show | Where |
|---|---|
| Task list + priorities + reasons | `source/main.c:131`, `source/app_config.h:77` |
| Game task: one blocking point (queue with timeout) | `source/game_task.c:141` |
| Sensor task blocking on a notification | `source/ultrasonic.c:194` |
| The only busy wait, justified | `source/ultrasonic.c:147` |
| Feedback task (owner of TPM0/TPM2) | `source/feedback.c:221` |
| Game state machine (pure C, unit-tested) | `source/game/game.c:425` |
| Stack overflow / malloc hooks | `source/main.c:57` |
| ESP32 task priorities | `esp32/.../config.h:56` |

## Synchronization tools (2) — D
**Need:** at least 1 mutex/semaphore, at least 1 queue.
- **Mutex:** game-state snapshot, `source/app_state.c:29`. A mutex, not a critical section (that would
  mask the UART ISR) and not a binary semaphore (a mutex has priority inheritance).
- **Queue:** the input queue every ISR and sensor task posts to, `source/input_queue.c:20`. Also the
  UART RX, TX-request, control and feedback queues.
- **Task notifications** as lightweight semaphores: ADC ISR to joystick, TPM1 ISR to ultrasonic.
- **ISR/task shared buffer:** TX ring in a short critical section, `source/uart_link.c:140`.
- **ESP32:** I2C bus mutex `esp32/.../i2c_bus.cpp:23` (plus app state, flash, timetable mutexes).

## RTOS enabled (2) — D
**Need:** time slicing and preemption enabled.
- `freertos/freertos-kernel/template/ARM_CM0/FreeRTOSConfig_Gen.h:44`: `configUSE_PREEMPTION 1`, and
  `configUSE_TIME_SLICING 1` a few lines below (was 0 in the template). Tick 200 Hz.
- Removed: software timers, counting/recursive semaphores, queue sets, trace, co-routines, etc.
- **Where slicing happens:** comm and ultrasonic share priority 2 on the MCX; network and web share 1
  on the ESP32.

## ESP32 use (7) — B
**Need:** meaningful use, more than digitalRead/Write and analogRead/Write, libraries beyond the template.
- 4 FreeRTOS tasks; OLED graphics; TLS HTTPS; JSON; flash storage (Preferences); a web server with mDNS;
  the alarm scheduler (first class - lead time); difficulty rule; proven puzzle bank.

| Show | Where |
|---|---|
| Tasks + libraries | `esp32/defuse_esp32/defuse_esp32.ino:38` |
| Link task (owns the protocol) | `esp32/.../link_task.cpp:386` |
| OLED drawing | `esp32/.../display_task.cpp:700` |

## Communications Protocol(s) (5) — C, B drives /eng
**Need:** good 2-way communication; 2 marks for packetization / control flow beyond Lab 3.
- **Frame:** `[0xAA][TYPE][SEQ][LEN][PAYLOAD 0-32][CRC-8]`. **Reliability:** ACK, retransmit after
  100 ms (max 3), NACK on bad CRC, duplicate drop by SEQ, 1 s heartbeat, link down after 3 s.
- **2-way:** ESP32 -> MCX: TIME_SYNC, BOMB_CONFIG, SET_ALARM, CMD. MCX -> ESP32: GAME_STATE, RESULT,
  TELEMETRY, BOMB_REPORT.
- **Live demo:** /eng > "Corrupt next packet" > Ring now: frame log shows CRC CORRUPTED, NACK, RETX, ACK.

| Show | Where |
|---|---|
| CRC-8 + frame format | `source/shared/protocol.c:32` |
| Decoder with resync | `source/shared/protocol.c:132` |
| ACK / retransmit / dup / heartbeat overview | `source/shared/proto_link.c:65` |
| Lost frame -> resend, give up after 3 | `source/shared/proto_link.c:140` |
| Heartbeat + link down | `source/shared/proto_link.c:308` |
| What the MCX receives / sends | `source/uart_link.c:169` |
| Comm task | `source/uart_link.c:340` |
| Application-level confirmation (BOMB_REPORT) | `source/game_task.c:58` |
| Bad config rejected / clamped | `source/game/game.c:473` |
| ESP32 send order TIME_SYNC, BOMB_CONFIG, SET_ALARM | `esp32/.../network_task.cpp:401` |
| Fault injection (bad CRC on purpose) | `esp32/.../link_task.cpp:103` |

## Online function(s) (5) — C
**Need:** non-trivial online use; 2 marks for GenAI / database / web application.
- All in the lowest-priority network task, cached, with offline fallbacks; never needed at alarm time.

| Function | Where |
|---|---|
| Overview of all online functions | `esp32/.../network_task.cpp:758` |
| REST: NUSMods timetable GET | `esp32/.../timetable.cpp:144` |
| GenAI: SoCLaaS chat call | `esp32/.../llm_client.cpp:101` |
| GenAI use: morning briefing + coach | `esp32/.../briefing.cpp:208` |
| Database: Firebase write | `esp32/.../mirror.cpp:50` |
| Web application: device web page + JSON API | `esp32/.../web_task.cpp:871` |

**If asked "why not AI puzzles?"** Board tests showed timeouts and wrong answers on hard riddles, so
puzzles come from the ESP32's bank (proven unique by brute force), and the LLM writes the briefing
(`APP_PUZZLE_USE_LLM 0` in `config.h`).

---

## 30-second check before the TA arrives
- [ ] Laptop on `qna-notes`, MCUXpresso open with `source/main.c`, `source/app_config.h`,
      `FreeRTOSConfig_Gen.h`, `source/uart_link.c` and `source/app_state.c` in tabs.
- [ ] Browser on `http://defuse.local/eng` for the protocol demo.
