# Demo script: Defuse to Wake (B02 group 10), 5 to 7 minutes

Rubric (35): sensors/actuators 5, polling + interrupts 2, ISR design 3, task design 4, sync 2,
RTOS config 2, ESP32 use 7, comms protocol 5, online 5. Each step below names the line it earns.
Code state: commit d1d3689. Paths: S = mcxc444/defuse_mcx/source, E = esp32/defuse_esp32/src/app.

## Before the TA arrives (setup, about 10 min)

- Flash both boards (MCX via MCUXpresso, ESP32 with the command at the end): the config-sync fix changed both.
- Power both boards on separate USB cables with the grounds tied together. Join the phone hotspot and
  wait for the OLED IDLE screen to show the IP.
- Laptop tab 1: http://defuse.local/ (or the IP). Tab 2: http://defuse.local/eng.
- Set Difficulty to **Normal** on the main page so the time limit is predictable. Every demo bomb is
  recorded in history, and Auto would change the level.
- Check /eng: frame log scrolling, heartbeats ACKed; the "This morning" card shows an LLM briefing.
- Open these files in the IDE, one tab each, in this order (the code walk follows it):
  1. FreeRTOSConfig_Gen.h
  2. S/app_config.h
  3. S/main.c
  4. S/uart_link.c
  5. S/encoder.c
  6. S/joystick.c
  7. S/ultrasonic.c
  8. S/app_state.c
  9. S/game_task.c
  10. shared/proto_link.c
  11. E/config.h
  12. E/network_task.cpp
- Who says what (each person owns the rubric lines of their block and takes the TA's follow-up questions on it):

  | Who | Block | Talks through | Marks | Also does |
  |---|---|---|---|---|
  | A: presenter | 0:00-0:40, 6:45-7:00 | Pitch, architecture, NUSMods grid, close | Online (REST) | Plays the bomb in B's block |
  | B: live bomb | 0:40-3:15 | Demo alarm, one sentence per module, strike feedback, briefing | Sensors/actuators 5, poll + IRQ 2, ESP32 7, GenAI | Drives the laptop for C |
  | C: protocol + online | 3:15-4:30 | /eng frame log, corrupt packet, BOMB_REPORT confirmation, outage + Ring now, Firebase | Comms 5, Online 5 | Protocol questions |
  | D: code walk | 4:30-6:45 | The code table below | ISR 3, task 4, sync 2, RTOS config 2 | "Likely TA questions" |

  With five people: D does the MCX side of the code walk (ISRs, tasks, RTOS config), E does shared/ and the
  ESP32 rows and shows snooze if time allows. Give the code walk to whoever knows the code best: it is the
  longest block and draws the most questions.

## 0:00 to 0:40 - Pitch and architecture (A) (no marks on their own, but they frame everything)

"Defuse to Wake reads our NUSMods timetable and, before the first class, arms a bomb you can only
stop by solving four sensor modules. The MCXC444 runs FreeRTOS and owns every sensor and actuator.
The ESP32-S2 draws the OLED and does all the online work. Design rule: the alarm never needs the
internet. Everything online is fetched ahead of time and has an offline fallback."

Show the main page: the timetable grid from NUSMods, the next alarm = first class minus the lead
time. **Online 5 (REST)**

## 0:40 to 3:15 - Live bomb, B talks, A plays (sensors/actuators 5, poll + IRQ 2, ESP32 use 7)

1. On the main page set a **demo alarm, 1 min**. Say: "This goes through the same path as a real
   morning: the ESP32 sends SET_ALARM, the MCX arms itself and rings on its own clock." The countdown
   shows the demo badge. While waiting, B says the module order is shuffled per bomb, and each
   puzzle pair is used once and never repeats.
2. It rings: buzzer, red LED, OLED bomb screen with the timer. **Actuators**
3. Play the modules in the order they appear. Say one sentence per module:
   - **Dial**: "The rotary encoder is interrupt driven: every A/B edge fires the PORTA interrupt."
     Read the clues aloud: "The ESP32 generated these itself: it picked the code, built the clues and
     brute-forced all 10,000 codes to prove there is exactly one answer, at the difficulty we asked for."
   - **Wires**: "The joystick is polled at 50 Hz, the cut button is an interrupt." Cut a wrong wire on
     purpose → strike: buzzer, LED colour, hint appears on the OLED. **Actuators + feedback**
   - **Tripwire**: "Ultrasonic: the trigger is periodic, the echo is timed by TPM input capture in an
     interrupt. Hold your hand in the band for 3 s."
   - **Lights**: "The LDR is polled at 10 Hz in the same task, because one task owns the ADC. Torch on."
4. Defused: jingle, green LED, then the briefing and coach pages (LLM-written) on the OLED.
   **Online 5 (GenAI)**
5. If time allows, long-press the encoder during the next bomb to show snooze (+5 min, +1 module).

## 3:15 to 4:30 - Protocol and fault injection on /eng, C talks, B drives (comms 5, online 5)

1. Show the frame log: type, SEQ, ACK, and 1 s heartbeats. "Frame = 0xAA, type, SEQ, length,
   payload, CRC-8."
2. Click **Corrupt next packet**. The next ESP32→MCX frame shows NACK then a retransmit with the
   same SEQ. "Stop-and-wait: 100 ms timeout, 3 retries, duplicates are re-ACKed, not re-delivered."
2b. Say: "An ACK only means the bytes arrived. The MCX also confirms at the application level: after every
   BOMB_CONFIG it sends a BOMB_REPORT saying what it holds, and when a bomb starts, what it runs. The OLED draws
   only from those, so the screen can never disagree with the game." Point at a BOMB_REPORT row in the frame log.
3. Puzzle panel: the used-pair mask and "LLM puzzles off: built-in bank". "We tried LLM-written riddles
   checked by a second model; on the board they failed too often, so the LLM only writes the briefing."
4. Click **Simulate internet outage** (60 s), then press **Ring now**. The bomb still runs, from the
   with its generated puzzles. "No alarm or game step waits on the internet. Puzzles are generated on
   the ESP32, and the briefing falls back to a template and is retried every 60 s while online."
5. Mention the Firebase mirror queue (write-only, retried). **Online 5 (DB)**

## 4:30 to 6:45 - Code walk, D (ISR 3, task 4, sync 2, RTOS config 2)

TAs will pick things to inspect; lead them with this table.

| Claim | Where |
|---|---|
| Preemption + time slicing on | FreeRTOSConfig_Gen.h:44 (PREEMPTION 1), :60 (TIME_SLICING 1), :48 (MAX_PRIORITIES 5), :47 (tick 200 Hz) |
| Unused features off | same file :89 timers 0, :56 counting sems 0, :55 recursive mutex 0, :59 queue sets 0; :54 mutexes 1 (we use one) |
| Task priorities and why | S/app_config.h:78-84: game 4 > joystick 3 > comm 2 = ultrasonic 2 (equal on purpose, time slicing) > feedback 1 = debug 1 |
| Tasks created, scheduler started | S/main.c:160-166, vTaskStartScheduler :171 |
| Game task blocks on one queue | S/game_task.c:169 xQueueReceive (single blocking point); the FSM in game/game.c |
| UART RX ISR | S/uart_link.c:109 handler, :121 xQueueSendFromISR, :136 portYIELD_FROM_ISR, NVIC prio 64 at :103 |
| Echo capture ISR | S/ultrasonic.c:98 TPM1 handler, :122/:135 vTaskNotifyGiveFromISR, :140 yield, prio 64 at :75 |
| ADC done ISR | S/joystick.c:79, notify :86, yield :89, prio 128 at :72 |
| Encoder ISR | S/encoder.c:75 PORTA handler → input_queue.c:26 xQueueSendFromISR, yield :106, prio 128 at :64 |
| Buttons ISR + debounce | S/buttons.c:76 handler, debounce 30 ms :60-66, long press 800 ms :90-93, prio 128 at :55 |
| ISR priority reasoning | S/app_config.h:72-75: UART and echo at 64 (bytes and edges can't wait), ADC and GPIO at 128 (human speed) |
| No printf/blocking in ISRs | every handler: read/clear hardware, FromISR call, yield. CM0+ masks with PRIMASK (comment at uart_link.c:102, encoder.c:63) |
| Polling with vTaskDelayUntil | S/joystick.c:189 (20 ms = 50 Hz), LDR every 5th loop :197-201; S/ultrasonic.c:198 (60 ms), echo wait with 35 ms timeout :216 |
| Mutex | S/app_state.c:21 (the only mutex: shared game state), bounded take :42/:54 |
| Queues | ctrl S/app_state.c:24, input S/input_queue.c:11, feedback S/feedback.c:252, UART rx/tx S/uart_link.c:362-363 |
| No producer can stall | S/input_queue.c:37-39: zero-timeout send plus a drop counter |
| Protocol | shared/protocol.c:32 CRC-8, :113 CRC check; shared/proto_link.h:36-38 (100 ms, 3 retries, 1000 ms heartbeat); proto_link.c:140 retry_or_fail, :89-112 ACK/NACK, :263 duplicate drop, :65 separate seq counter for reliable frames |
| ESP32 tasks (single core, so priority is the schedule) | E/config.h:56-60: link 4 > display 2 > network 1 = web 1; created in defuse_esp32.ino:54-58 |
| ESP32 never blocks the game | E/network_task.cpp:487 service_llm_jobs (one LLM job per pass, 20 s timeout per call), :452 puzzle_calls_allowed (no calls in a session or near the alarm) |
| GenAI (briefing + coach) | E/briefing.cpp:299 briefing_service, :208 llm_chat (qwen first, APP_BRIEF_CHAIN in E/config.h), bad replies become a template; LLM puzzles off: APP_PUZZLE_USE_LLM in E/config.h |
| Puzzle generator (no LLM) | E/puzzle_bank.cpp:385 bank_generate: random clues true at the ESP32's answer, brute-force proof of exactly one solution, level rated by the solver in E/puzzle_rules.h |
| Random order + pre-shifted dial | E/puzzle_logic.cpp:926 module_order_shuffle, :961 dial_code_preshift |
| REST | NUSMods GET E/timetable.cpp:170 (URL E/config.h:162); SoCLaaS POST E/llm_client.cpp:143; Firebase POST E/mirror.cpp:76; NTP E/network_task.cpp:512 |
| App-level confirmation (BOMB_REPORT) | MCX sends: S/game_task.c:58 flush_reports, :100 HELD after each BOMB_CONFIG, :212 RUNNING at bomb start; CMD_REPORT routed S/uart_link.c:217. ESP32: E/link_task.cpp:140 receives, :321 asks at link-up; E/cfg_sync.h matching rules |
| Web server routes | E/web_task.cpp:883-895 |

Line numbers are as of commit d1d3689 and S/app_config.h as on disk (your uncommitted edits). If
you change a file before the demo, re-check its numbers.

## 6:45 to 7:00 - Close (A)

"Four sensors, three actuators, five ISRs feeding queues and notifications into six tasks with
deliberate priorities, a reliable framed protocol between the boards, provably correct generated
puzzles, and an LLM briefing that always has an offline fallback."

## Likely TA questions (have one answer ready each)

- *Why is the joystick polled but the encoder interrupt-driven?* The encoder makes fast edges we
  must not miss. The joystick is a level we only need at a fixed 50 Hz, and vTaskDelayUntil keeps
  that rate without jitter.
- *Why are comm and ultrasonic at the same priority?* Neither is more urgent than the other, so time
  slicing shares the CPU between them. It also shows time slicing working.
- *What happens if the ESP32 dies mid-bomb?* The MCX runs the bomb on its own: the timer, the
  answers and feedback are all local. The link goes down after 3 s of missing heartbeats.
- *Priority inversion?* There is one mutex with short, bounded holds. FreeRTOS mutexes use
  priority inheritance.
- *Why is the UART above GPIO?* At 115200 baud a byte arrives every 87 us, and the RX register must
  be read before the next byte or it overruns. Button and encoder edges come at human speed.
- *How do you know a puzzle is right?* The ESP32 chose the answer and generated the clues itself,
  brute-forcing all 10,000 codes to prove exactly one solution. *Why not LLM puzzles?* We built that
  (LLM writes, a second model solves blind); on the board the models timed out or solved Hard puzzles
  wrong, so the LLM now only writes the briefing and coach line. The code is still behind a flag.

## Rehearse

Run the whole script at least twice with a stopwatch. If the demo alarm takes too long, use Ring now
for the second bomb. Rehearsal bombs are recorded in history too (Serial `hist` lists them), so
keep Difficulty on Normal so they don't change the level.
