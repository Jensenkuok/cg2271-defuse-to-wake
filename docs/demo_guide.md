# Defuse to Wake: demo guide (where every behaviour lives in the code)

How to use this: each section is one thing you show or get asked about at the demo. It says
**what to say**, **which file and line to open**, the **key lines**, and the **likely TA question**.
Line numbers are for `master` at `1e394ba` (Ryan's code as of 27 Oct). If the code changes, search
for the function name instead.

Section order follows the rubric: sensors/actuators → polling + interrupts → ISRs → tasks → sync → RTOS config →
ESP32 → comms protocol → online.

---

## 0. The 30-second pitch and the big picture

> "Defuse to Wake is a lecture alarm. The ESP32 reads my NUSMods timetable and schedules an alarm before
> my first class. At wake time the MCXC444 arms a bomb that keeps ticking until I clear four sensor
> puzzles: a safe dial, a wire cutter, a distance tripwire and a light sensor. The puzzles are
> AI-written the night before and verified by a second AI. The bomb never needs the internet."

| Board | Job | Why this board |
|---|---|---|
| MCXC444 (FreeRTOS, register-level C) | Game engine + all 4 sensors + buzzer + RGB LED | Real-time: interrupts, timer capture and PWM need exact timing, and the alarm must work with no Wi-Fi |
| ESP32-S2 (Arduino on FreeRTOS) | OLED, Wi-Fi, NTP, NUSMods, LLM, Firebase, web page | Has Wi-Fi + TLS + lots of RAM for libraries and the screen framebuffer |

Start-up code to point at:
- MCX: `mcxc444/defuse_mcx/source/main.c:146-171`. Create the queues and peripherals first, then the tasks,
  then `vTaskStartScheduler()`. If anything fails, `fatal_halt()` turns the red LED on solid.
- ESP32: `esp32/defuse_esp32/defuse_esp32.ino:54`. `setup()` only creates tasks; `loop()` deletes itself.

---

## 1. Sensors and actuators (5 marks: 3 for the count, 2 for justification)

| # | Part | Board | Polled / IRQ | Pins | Code |
|---|---|---|---|---|---|
| S1 | Rotary encoder + push | MCX | **Interrupt** (A/B edges, push) | PTA12, PTA5, PTC1 | `encoder.c`, `buttons.c` |
| S2 | PS2 joystick + button | MCX | **Polled** 50 Hz (X), button **interrupt** | PTE20 (ADC0_SE0), PTC2 | `joystick.c`, `buttons.c` |
| S3 | HC-SR04 ultrasonic | MCX | Periodic trigger, echo by **timer input capture interrupt** | PTB3 TRIG, PTA13 ECHO (TPM1_CH1) | `ultrasonic.c` |
| S4 | KY-018 LDR light sensor | MCX | **Polled** 10 Hz | PTB0 (ADC0_SE8) | `joystick.c` (`ldr_sample`) |
| A1 | Passive buzzer | MCX | TPM2 PWM | PTB2 | `feedback.c` |
| A2 | RGB LED (on-board) | MCX | TPM0 PWM | PTE31/PTD5/PTE29 | `feedback.c` |
| A3 | SSD1306 OLED | ESP32 | I2C 0x3C | GPIO8 SDA, GPIO9 SCL | `display_task.cpp` |
| (opt) | MPU-6050 gyro (tamper) | ESP32 | Polled 50 Hz | I2C | `gyro_task.cpp`, **off by default** (`config.h:29` `APP_USE_GYRO 0`) |

Every pin, with page references to the manuals: `docs/pin_table.md`. All pin numbers are in
`mcxc444/defuse_mcx/source/app_config.h` (no magic numbers in the .c files).

**Justification lines to say (this is where the 2 extra marks are):**
- *Encoder → interrupt:* quadrature edges are short and fast. Polling would miss steps when the knob spins
  quickly. Each edge is decoded instantly in the ISR.
- *Joystick → polled:* it's an analog voltage with no "event" to interrupt on. You sample it at a fixed rate.
  50 Hz gives smooth cursor movement without wasting CPU.
- *Ultrasonic → timer capture:* the echo pulse width is distance (58 µs per cm). The TPM hardware latches
  the counter value at the edge, so the measurement is exact even if a task is running.
- *LDR → polled at 10 Hz:* light changes slowly, so 10 Hz is enough. It shares the joystick task so only
  one task ever touches ADC0.
- *On-board limit:* only one on-board actuator (RGB LED) and one on-board input (SW3, a bench trigger) are used.
  The board's own light sensor is NOT used; the LDR is the external KY-018.

---

## 2. Polling and interrupts (2 marks)

### Polled: joystick + LDR, `joystick.c:179`
```c
for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(APP_JOY_PERIOD_MS));   // 20 ms = 50 Hz, fixed rate
    if (adc_read(APP_JOY_X_ADC_CH, &x) && adc_read(APP_JOY_Y_ADC_CH, &y)) { ... x_to_events(x, &arm); }
    if (++loops >= APP_LDR_EVERY_N) { loops = 0u; ldr_sample(); }  // every 5th loop = 10 Hz
}
```
- `vTaskDelayUntil` (not `vTaskDelay`) keeps the period exact: it measures from the last wake, not from now.
- `adc_read` (`joystick.c:93`) starts a conversion and **blocks on a task notification** that the ADC
  ISR gives. It does not spin on the COCO flag.
- `x_to_events` (`joystick.c:106`) uses **hysteresis**: one LEFT/RIGHT event per push, re-armed only when
  the stick comes back to the centre.

### Interrupt-driven: encoder, `encoder.c:75`
- Gray-code lookup table `k_transition[16]` (`encoder.c:21`): one step per detent, and contact bounce cancels out.
- On a full detent it posts `GAME_EVT_ENC_STEP ±1` to the input queue.

### Interrupt-driven: ultrasonic echo, `ultrasonic.c:98` (ISR) + `ultrasonic.c:191` (task)
- The task fires a 10 µs trigger (`trigger_pulse`, `ultrasonic.c:147`) every 60 ms, then **blocks** on a
  notification with a 35 ms timeout.
- The ISR stores the rising-edge capture, computes `fall - rise` on the falling edge, and notifies the task.
- The task converts µs to cm (`/58`) and applies a median of 3 (`median_filter`, `ultrasonic.c:165`) so one lost echo is ignored.

---

## 3. ISR design (3 marks)

Rule (say it): **"Every ISR reads the hardware, clears the flag, hands the data to a task with a
`...FromISR` call, and ends with `portYIELD_FROM_ISR`. No printf, no delays, no waiting for input."**

| ISR | File:line | What it does | Hands off with |
|---|---|---|---|
| `UART2_FLEXIO_IRQHandler` | `uart_link.c:109` | RX: one byte into the RX queue. TX: feeds the next byte from a ring, then turns off TIE when empty | `xQueueSendFromISR` |
| `TPM1_IRQHandler` | `ultrasonic.c:98` | Echo edge timestamps | `vTaskNotifyGiveFromISR` |
| `ADC0_IRQHandler` | `joystick.c:79` | Reads the result (clears COCO) | `vTaskNotifyGiveFromISR` |
| `PORTA_IRQHandler` | `encoder.c:75` | Quadrature decode + SW3 | `xQueueSendFromISR` (via `input_post_from_isr`) |
| `PORTC_PORTD_IRQHandler` | `buttons.c:76` | Encoder push (short/long), joystick press, optional shock switch | `xQueueSendFromISR` |

The UART ISR, the best one to show (`uart_link.c:109`):
```c
if (s1 & (UART_S1_RDRF_MASK | UART_ERROR_FLAGS)) {
    byte = UART2->D;                                   // read clears RDRF + error flags
    if (RDRF && !FE) xQueueSendFromISR(s_rx_queue, &byte, &woken);   // parse later, in the comm task
}
if (TIE && TDRE) { send next byte from ring, or disable TIE when empty }  // interrupt-driven TX too
portYIELD_FROM_ISR(woken);
```
**Debouncing inside an ISR without waiting** (`buttons.c:61` `accept_edge`): it compares the tick count
from `xTaskGetTickCountFromISR()` with the last accepted edge and ignores edges within 30 ms. Long press
(≥ 800 ms) is measured the same way: press time stored, decided on release. Nothing ever loops.

**ISR priorities** (`app_config.h:72-75`), Cortex-M0+ has 2 bits: 0/64/128/192:
- UART2 = 64, TPM1 = 64: a missed UART byte means a lost frame, and a late capture means a wrong distance.
- ADC0 = 128, GPIO = 128: human-speed events can wait a few µs.

**Likely TA question:** *"Why can you call FromISR at priority 64?"* → On the CM0+ FreeRTOS port, the
kernel masks interrupts with PRIMASK (all or nothing), not BASEPRI, so `...FromISR` is safe at any
priority. The comment is at `uart_link.c:100-102`.

---

## 4. Task design (4 marks)

### MCX tasks (`main.c:1-15` header comment, priorities in `app_config.h:77-84`)
| Task | Prio | File | Blocks on | Why this priority |
|---|---|---|---|---|
| game | 4 | `game_task.c:69` | input queue with timeout to the next 50 ms tick | Player input must be handled at once |
| joystick | 3 | `joystick.c:179` | `vTaskDelayUntil` 20 ms + ADC notification | Fixed-rate sampling must not jitter |
| comm | 2 | `uart_link.c:332` | RX queue, 10 ms timeout | Must poll retransmit timers often, but is not game-critical |
| ultrasonic | 2 | `ultrasonic.c:191` | `vTaskDelayUntil` 60 ms + echo notification | **Same as comm on purpose: shows time slicing** |
| feedback | 1 | `feedback.c:219` | `vTaskDelayUntil` 10 ms | A sound 10 ms late is not noticeable |
| debug | 1 | `debug.c` | queue/notification | Prints only, lowest |

> ⚠ `app_config.h:78` still says the game task is "(not created yet)". It IS created (`game_task.c`).
> Fix this comment before the demo, because a TA could read it.

### The game task loop, the heart of the MCX (`game_task.c:69-145`)
1. `xQueueReceive(input_queue, &ev, wait)` (line 98), where `wait` = time until the next tick. It sleeps when idle.
2. Drain control messages from the ESP32 (TIME_SYNC / SET_ALARM / BOMB_CONFIG) → `apply_ctrl` (line 26).
3. Every 50 ms: `game_tick()` (timer, tripwire/light hold, state timeouts).
4. `publish()` (line 51): copy the view into the shared snapshot **under the mutex**.
5. Send LED mode / tick rate / sounds to the feedback task (queue), and RESULT to the ESP32 (line 138).

**"Yields to other tasks":** every task blocks on a queue, a notification or `vTaskDelayUntil`. The only
busy wait is the 10 µs trigger pulse (`ultrasonic.c:147`), shorter than one 5 ms tick and documented.

### The game engine, pure C, no RTOS (`game/game.c`)
TAs like this split: **game logic is hardware-independent and unit-tested on a PC** (`tools/test_game.c`, 353 checks).

| Behaviour | Function | Line |
|---|---|---|
| Alarm deadline → bomb starts, extra modules after snooze/explosion | `enter_bomb` | 187 |
| Strike, explode at max strikes | `add_strike` | 236 |
| Dial: digit right → next, wrong → strike + reset | `dial_press` | 272 |
| Wires: correct cut clears, wrong = strike, already cut = denied | `wire_press` | 303 |
| Tripwire: hand in 15–25 cm band for 3 s (300 ms grace) | `trip_distance` + `game_tick` | 328, 571 |
| Lights: baseline + 40 levels for 2 s (shine a phone torch) | `light_level` + `game_tick` | 356, 571 |
| Tamper ≥ 1500 mg = strike, 2 s cooldown | `tamper` | 385 |
| Snooze: +5 min, max 2, +1 module | `snooze` | 398 |
| Inputs only affect the current module | `module_event` | 423 |
| Every ESP32 config value range-checked | `game_set_config` | 471 |
| Ticking speeds up in the last 10 s | `game_tick_interval_ms` | 629 |

State machine: IDLE → ARMED → BOMB_ACTIVE → DEFUSED / EXPLODED (→ new bomb +1 module after 3 s).
Diagram: `docs/game.md`.

---

## 5. Synchronisation tools (2 marks)

### Mutex: shared game snapshot (`app_state.c:29-63`)
- Writer: game task (prio 4). Readers: comm (2) and debug (1).
- **Why a mutex and not a critical section** (the comment at line 29 is great to read out):
  a critical section would mask all interrupts, including the UART ISR. A mutex only blocks the tasks.
- **Why a mutex and not a binary semaphore:** priority inheritance. If debug (1) holds it and comm (2)
  preempts debug, the waiting game task (4) lends its priority to debug. That prevents priority inversion.
- Every take has a bounded 5 ms timeout (`APP_STATE_MUTEX_WAIT_MS`), so a task never hangs forever.

### Queues (MCX)
| Queue | Producer → consumer | Created at |
|---|---|---|
| Input events (16) | all ISRs + joystick/ultrasonic tasks + comm → **game** | `input_queue.c:11` |
| UART RX bytes (128) | UART ISR → comm | `uart_link.c:362` |
| TX requests (4) | game → comm (owns the link) | `uart_link.c:363` |
| Control (4) | comm → game (TIME_SYNC, SET_ALARM, BOMB_CONFIG) | `app_state.c:24` |
| Feedback (8) | game → feedback (sound, LED, tick) | `feedback.c:252` |

Plus **task notifications** as lightweight binary semaphores (ADC ISR → joystick, TPM1 ISR → ultrasonic).

### ESP32 (bonus)
Mutexes on the I2C bus (`i2c_bus.cpp:23`, OLED + gyro share it, and priority inheritance matters
because they have different priorities), app state (`app_state.cpp:26`), flash (`flash_store.cpp:12`), and more.
Queues: TX queue to the link task (`link_task.cpp:188`) and result queue to the network task.

---

## 6. RTOS configuration (2 marks)

Open `mcxc444/defuse_mcx/freertos/freertos-kernel/template/ARM_CM0/FreeRTOSConfig_Gen.h`.
Every change is marked `defuse:` with a reason.

| Line | Setting | Say |
|---|---|---|
| 44 | `configUSE_PREEMPTION 1` | A higher-priority task that becomes ready runs immediately |
| 60 | `configUSE_TIME_SLICING 1` (was 0) | Equal-priority ready tasks (comm + ultrasonic at 2) share the CPU every tick |
| 47 | `configTICK_RATE_HZ 200` | 5 ms tick |
| 55, 56, 59, 81, 85, 89 | recursive mutex, counting sem, queue sets, trace, co-routines, **timers** = 0 | "Removed unnecessary components": no timer daemon task, less RAM |
| 75, 76 | stack overflow check 2, malloc-failed hook | Hooks in `main.c:57` light the red LED and halt |

> Warning: never click "Update Code" in Config Tools, because it regenerates this file (`mcxc444/README.md`).

---

## 7. ESP32 use (7 marks)

Tasks (`esp32/defuse_esp32/src/app/config.h:56-60`, single core, no pinning):
| Task | Prio | File:line | Job |
|---|---|---|---|
| link | 4 | `link_task.cpp:353` | Only owner of the UART link. Runs every 5 ms (`vTaskDelayUntil`), sends BOMB_CONFIG, BOOT-button demo ring |
| gyro | 3 | `gyro_task.cpp:241` | MPU-6050 at 50 Hz → TAMPER_EVENT (off by default) |
| display | 2 | `display_task.cpp:717` | OLED at 10 fps |
| network | 1 | `network_task.cpp:595` | Wi-Fi, NTP, NUSMods, LLM, Firebase, alarm scheduling |
| web | 1 | `web_task.cpp:741` | Local web server |

Why the link task is highest: if Serial1 isn't drained the RX buffer overflows and ACKs miss the 100 ms
window. Why network is lowest: an LLM call can block for up to 30 s, and nothing waits on it.

Things beyond `digitalRead/analogWrite` to point at:
- **OLED rendering** (`display_task.cpp`): `build_model` (line 223) turns the state into a screen model,
  `draw_oled` (656) draws the dial/wires/tripwire/lights graphics in RAM, and only then takes the I2C mutex
  for `s_display.display()` (786). It falls back to a Serial summary if no OLED answers.
- **Libraries:** WiFi, WiFiMulti, NetworkClientSecure (TLS), HTTPClient, ArduinoJson, Preferences (flash),
  WebServer, ESPmDNS, Adafruit_SSD1306/GFX, Wire.
- **Scheduling logic:** `schedule.cpp:306` `next_alarm()` = first class of the day − lead time, using the
  NUS semester calendar (recess week handled). Host-tested (`tools/test_schedule.cpp`).
- **Results in flash:** the last 14 mornings (`history.cpp:49`), the source of truth for difficulty and the web page.

---

## 8. Communications protocol (5 marks: 3 + 2 for beyond-lab UART)

Spec: `docs/protocol.md`. Code: `shared/protocol.c` + `shared/proto_link.c`. **The same files compile
unchanged on both boards** (synced copies, `tools/sync_shared.ps1`).

**Packetisation:** `[0xAA][TYPE][SEQ][LEN][PAYLOAD 0–32][CRC-8]`
- `proto_encode`: `protocol.c:50`; CRC-8 polynomial 0x07: `protocol.c:32`.
- Decoder takes one byte at a time (`proto_decoder_feed`, `protocol.c:132`) and resyncs on the next 0xAA after a bad frame.

**Two-way data:**
| ESP32 → MCX | MCX → ESP32 |
|---|---|
| TIME_SYNC, SET_ALARM, BOMB_CONFIG (puzzle answers, time, modules), CMD (ring/snooze/cancel), TAMPER_EVENT | GAME_STATE (≤ 10/s), RESULT (defused/exploded), TELEMETRY (raw sensors, 2/s) |
| Both: HEARTBEAT (1 s), ACK, NACK | |

Where they are handled: MCX `route_frame` (`uart_link.c:169`) → control queue / input queue.
ESP32 `link_deliver` (`link_task.cpp:129`) → app state, and RESULT → queue to the network task (never
write flash from the prio-4 task, `link_task.cpp:151`).

**Flow control / reliability (`proto_link.c`):**
| Mechanism | Where |
|---|---|
| Reliable types are ACKed; stop-and-wait, one outstanding + FIFO of 4 | `proto_link_send` 171, `handle_ack` 201 |
| No ACK in 100 ms → retransmit, max 3, then SEND_FAILED | `proto_link_poll` 291, `retry_or_fail` 131 |
| NACK (bad CRC) → retransmit at once | `handle_frame` 214 |
| Duplicate (same SEQ) → re-ACK but don't deliver twice | line 247 |
| Heartbeat 1 s, link down after 3 s silence → LED blinks blue | `proto_link_poll` 291, `feedback.c` `update_led` |
| GAME_STATE rate-limited to 10/s | `uart_link.c:272` |

**Live demo of recovery:** web page `/eng` → "Corrupt next packet" (`web_task.cpp:650`) → the ESP32 write
callback flips the CRC of the next frame (`link_task.cpp:101`, `fault_corrupt_take` `fault.cpp:40`) → the MCX
NACKs → the ESP32 retransmits. It shows in the frame log on the same page.

---

## 9. Online functions (5 marks: 3 + 2 for GenAI / database / web app)

All run in the lowest-priority network task. **Everything is fetched ahead of time and cached, with a fallback.
The alarm never waits on the internet.**

| Service | What | Code | Data sent / received |
|---|---|---|---|
| **NUSMods REST** | Timetable | `timetable.cpp:144` `fetch_module`, `http.GET()` line 170 | GET `api.nusmods.com/v2/2026-2027/modules/<CODE>.json` → lessons, cached in flash |
| **SoCLaaS LLM (GenAI)** | Puzzles + morning briefing | `llm_client.cpp:101` `llm_chat`, Bearer header line 142 | POST OpenAI-style `/v1/chat/completions` (model, system, user, max_tokens) → JSON text |
| **Firebase Realtime DB** | Mirror of each morning's result | `mirror.cpp:50` `post_record`, `http.POST` line 76 | POST `mornings.json` with the record; retry with backoff |
| **NTP** | Wall clock | `network_task.cpp:439` `configTime` | UTC+8 |
| **Local web app** | Settings + live engineering view | `web_task.cpp:753` routes | `/api/status` JSON polled every 500 ms |

**The AI puzzle pipeline (the impressive bit, `puzzle_gen.cpp`):**
1. The ESP32 picks the answer itself (random 4-digit code / wire number). **The LLM never decides the answer.**
2. `step_setter` (line 435): the setter model writes a riddle for that answer. `gate_check_setter`
   (`puzzle_gate.cpp:66`) checks it with a brute-force solver: it must have exactly one solution, our answer.
3. `step_checker` (line 487): a **different** model gets only the riddle text (comment at line 502) and must
   answer with the same code.
4. Match → keep it. Otherwise retry, then fall back to the built-in procedural bank (`puzzle_bank.cpp`), which is always correct.
5. Model chain: `config.h:180`, setter = `qwen3.6:35b` → `x-test-1` → `x-test-2` → `default`.

Briefing: one LLM call (`briefing.cpp:182`) with a template fallback, shown on the OLED after defusing.
Difficulty: a fixed score from recent snoozes/strikes (`difficulty.cpp:39`), **not** the LLM.

**Live demo of resilience:** `/eng` → "Simulate internet outage" (`web_task.cpp:659`), which makes
`fault_net_blocked()` true for 60 s. Every HTTPS call fails fast. Then ring the bomb: it still works.

---

## 10. Suggested 6-minute demo script

1. **(30 s) Pitch** (section 0). Show the hardware and point at each sensor/actuator.
2. **(1 min) Web page** `http://defuse.local`: next alarm from NUSMods, history, settings.
3. **(2 min) Ring now** (BOOT button on the ESP32, SW3 on the MCX, or the web button) → play the bomb:
   - Dial: turn + press, enter one digit wrong on purpose → strike sound + LED + hint on the OLED.
   - Wires: joystick left/right, cut.
   - Tripwire: hand at 15–25 cm for 3 s.
   - Lights: phone torch on the LDR for 2 s → defuse jingle, green LED, AI briefing on the OLED.
4. **(1 min) Protocol:** `/eng` → Corrupt next packet → show NACK + retransmit in the frame log.
5. **(30 s) Outage:** Simulate outage → ring again → still works offline.
6. **(1 min) Code walkthrough** (TAs ask for this): `main.c` header → `app_config.h` priorities →
   `FreeRTOSConfig_Gen.h` lines 44/60 → `uart_link.c:109` ISR → `app_state.c:29` mutex comment.

Three ways to start a bomb, in case one fails: ESP32 BOOT button (`link_task.cpp:298` → `demo_ring`
line 314), MCX SW3 (`encoder.c`, ring in 3 s), web "Ring now".

---

## 11. Team split: who presents what, and who answers which questions

**Recommendation: split it, don't let one person do everything.** TAs ask questions to whoever is
standing there, so everyone needs to own a part they can explain from the code. One person talking
for 6 minutes also looks like the others didn't contribute. Keep two people fixed:
- **Operator:** runs the hardware (rings the bomb, plays it, presses the web buttons) the whole time.
  It never has to stop to talk. Make this person B below, who has the shortest speaking part.
- **Laptop:** one laptop with the code open in tabs (files listed per person below) and `/eng` open in a browser.
  Each speaker drives it during their part.

Each section is about one rubric line group, so marks map cleanly to people.

### Everyone must know (the shared core)
- The 30-second pitch and the two-board split with the reason (section 0).
- How to start a bomb three ways (BOOT, SW3, web "Ring now") and how to recover if one fails.
- The 4 sensors + 3 actuators and which board each is on (section 1 table).
- One sentence each on: ISR → queue → task; mutex = priority inheritance; frames with CRC + ACK;
  everything online is cached so the alarm works offline.
- If you don't know an answer: "that's [name]'s part, they can show you in the code." Don't guess.

### Person A: hardware, sensors, polling and interrupts (rubric: sensors 5, poll+IRQ 2, ISR 3 = 10 marks)
**Presents (~1.5 min, script steps 1 and 3):** pitch, walk round the hardware, then explain each module
while B plays the bomb.
**Master these files:** `docs/pin_table.md`, `app_config.h` (pins + NVIC priorities, lines 1-75),
`encoder.c:75`, `buttons.c:61-76`, `joystick.c:79-200`, `ultrasonic.c:98-230`, `uart_link.c:109`.
**Owns these questions:**
- Why is each sensor polled or interrupt-driven? (section 1 justification lines)
- Walk me through an ISR. (section 3, `uart_link.c:109`)
- How do you debounce without waiting? (`buttons.c:61` `accept_edge`)
- Why UART/TPM1 at 64 and GPIO/ADC at 128? Why is FromISR legal at 64? (section 3)
- How is distance measured? (TPM1 input capture, µs / 58, median of 3)
- Why the voltage divider on ECHO? (5 V signal into a 3.3 V pin, `docs/pin_table.md`)
- Which parts are on-board? (RGB LED + SW3 only; the LDR is the external KY-018)
**Backup for:** B.

### Person B: tasks, priorities, sync and RTOS config (rubric: task design 4, sync 2, RTOS 2 = 8 marks) + Operator
**Presents (~1 min, script step 6, first half):** `main.c` header → `app_config.h:77-84` priorities →
`FreeRTOSConfig_Gen.h` lines 44/60/89 → `app_state.c:29` mutex comment → `game_task.c:69` loop.
Operates the hardware for the rest of the demo.
**Master these files:** `main.c`, `app_config.h:77-110`, `FreeRTOSConfig_Gen.h`, `game_task.c`,
`app_state.c`, `input_queue.c`, `feedback.c:219`, `game/game.c` (function table in section 4), `docs/game.md`.
**Owns these questions:**
- List your tasks and priorities, and why each one. (section 4 table)
- Show me time slicing and preemption. (`FreeRTOSConfig_Gen.h:44,60`; comm = ultrasonic = 2)
- What did you remove from the config? (timers, counting/recursive semaphores, queue sets, trace, co-routines)
- Where is your mutex, and why not a semaphore or critical section? (`app_state.c:29`)
- Which queues, and who produces/consumes? (section 5 table)
- Does any task busy-wait? (no, only the 10 µs trigger pulse, `ultrasonic.c:147`)
- What happens on stack overflow? (`main.c:57`, red LED, halt)
- Explain the state machine / what happens on 3 strikes / snooze. (`game.c`, `docs/game.md`)
**Backup for:** A.

### Person C: inter-board protocol and the ESP32 tasks (rubric: comms 5 + ESP32 7 (shared with D) = ~9 marks)
**Presents (~1.5 min, script step 4 + ESP32 tasks):** frame format, ACK/retransmit, then the
"Corrupt next packet" demo with the frame log on `/eng`. Then the ESP32 task table and the OLED.
**Master these files:** `docs/protocol.md`, `shared/protocol.c` (32, 50, 132), `shared/proto_link.c`
(131-318), `uart_link.c:169` (`route_frame`), `uart_link.c:272-360`, `esp32/.../config.h:56-60`,
`link_task.cpp:101-160, 353`, `display_task.cpp:717`, `i2c_bus.cpp`.
**Owns these questions:**
- What's in a frame, and how is the CRC computed? (section 8, `protocol.c:32`)
- What data goes each way? (section 8 table)
- What happens if a frame is lost / corrupted / duplicated? (`proto_link.c` 131, 214, 247)
- How do you do flow control? (stop-and-wait + FIFO of 4, GAME_STATE ≤ 10/s, `link_tx_space` back-pressure)
- How do you know the other board is alive? (1 s heartbeat, link down after 3 s → blue LED blink)
- Why is the ESP32 link task the highest priority? Why is network the lowest? (section 7)
- How do the OLED and gyro share I2C? (mutex with priority inheritance, `i2c_bus.cpp:23`)
- Is the protocol the same on both boards? (yes, `shared/` compiled unchanged, `tools/sync_shared.ps1`)
**Backup for:** D.

### Person D: online functions and the web page (rubric: online 5 + ESP32 7 (shared with C) = ~9 marks)
**Presents (~1.5 min, script steps 2 and 5):** web page (alarm from NUSMods, history, settings), the AI
puzzle pipeline, Firebase, then the "Simulate internet outage" demo.
**Master these files:** `network_task.cpp:595` (loop), `network_task.cpp:299-380` (TIME_SYNC → SET_ALARM),
`timetable.cpp:144`, `schedule.cpp:306`, `llm_client.cpp:101`, `puzzle_gen.cpp:435-534`,
`puzzle_gate.cpp:66`, `briefing.cpp:182`, `difficulty.cpp:39`, `mirror.cpp:50`, `web_task.cpp:650-779`, `fault.cpp`.
**Owns these questions:**
- Which online services, and what data goes out and comes back? (section 9 table)
- How do you stop the LLM giving a wrong or unsolvable puzzle? (ESP32 picks the answer, solver proves it, a second model must solve it)
- Which models, and what if SoCLaaS is down? (`config.h:180` chain → built-in bank)
- What's stored in Firebase, and do you read it back? (morning records, write-only, retry with backoff)
- How is the alarm time computed? (first class − lead time, `schedule.cpp:306`)
- Why TIME_SYNC before SET_ALARM? (the MCX needs the clock to turn an alarm time into a deadline)
- What happens without internet? (cached timetable + puzzle bank + template briefing; outage button)
- How is the difficulty chosen? (fixed score from recent snoozes/strikes, not the LLM, `difficulty.cpp:39`)
- Where is the API key? (`secrets.h`, git-ignored, never printed, `llm_client.cpp:142`)
**Backup for:** C.

### If you have 5 people
Split Person A: **A1** = sensors/actuators + pins (sensors 5), **A2** = polling + ISRs (poll+IRQ 2, ISR 3).
Or give the game engine (`game/game.c`, `docs/game.md`, host tests) to person 5 and take it off B.

### Running order and timing (6 min)
| Time | Who | What |
|---|---|---|
| 0:00 | A | Pitch + hardware tour |
| 0:45 | D | Web page: next alarm, today's classes, history |
| 1:30 | B rings, A explains | Play the bomb: dial (one wrong digit), wires, tripwire, lights → defuse |
| 3:15 | C | Protocol + Corrupt next packet |
| 4:15 | D | AI pipeline + Simulate outage → B rings again, still works |
| 5:00 | B | RTOS code walkthrough |
| 6:00 | all | Questions: each person takes their own topics |

**Rehearse once with a timer**, and once where the others fire questions from this list at each person.

---

## 12. Quick answers to likely TA questions

| Question | Answer | Point at |
|---|---|---|
| Why is the game on the MCX, not the ESP32? | Real-time I/O, and the alarm must work with no Wi-Fi | Section 0 |
| Where is the ISR → task hand-off? | `xQueueSendFromISR` / `vTaskNotifyGiveFromISR` + `portYIELD_FROM_ISR` | `uart_link.c:109` |
| How do you show time slicing? | comm and ultrasonic are both priority 2; with slicing on they round-robin each 5 ms tick when both are ready | `app_config.h:80-81`, `FreeRTOSConfig_Gen.h:60` |
| Priority inversion? | Mutex (not semaphore) → priority inheritance | `app_state.c:29` |
| What if a frame is corrupted? | CRC fails → NACK → immediate retransmit; duplicates dropped by SEQ | `proto_link.c:214,247` |
| What if the ESP32 sends garbage config? | `game_set_config` rejects or clamps every field | `game.c:471` |
| What if the internet is down? | Cached timetable + puzzle bank + template briefing; Firebase queue retries | Outage button |
| Can the LLM give a wrong puzzle? | No: the ESP32 picks the answer, a solver proves it, a second model must solve it | `puzzle_gen.cpp:435,487` |
| Stack sizes / overflow? | Overflow check method 2 + hook; debug task prints the stack high-water marks | `main.c:57`, `debug.c:29` |
| Is it tested? | ~2.8 M host-test checks across 15 test programs (protocol, link, game, puzzles…) | `tools/` |
