# Game engine

Hardware-independent bomb game in `game/` (`game_config.h`, `game.h`, `game.c`). Pure C99, no
dynamic allocation, no globals, no OS calls, deterministic. The MCX game task feeds it events and
time and acts on its outputs. Host tests: `tools/test_game.c` (run `tools/run_tests.ps1`).
Rules are the team-approved ones in `CLAUDE.md`; every tunable number is a `#define` in `game_config.h`.

## Threading rule

A `game_t` is not thread-safe. One MCX task (the game task) owns it and makes every call,
including the getters. Other tasks and ISRs talk to that task through queues. Time is a `uint32_t
now_ms` argument and all comparisons are wraparound-safe. `game_tick` is called every 50 ms.
`game_set_alarm` has no `now_ms`, so it uses the `now_ms` of the latest call that had one (at most
one tick old).

## State diagram

```
            RING_NOW / set_alarm                 deadline passes (tick)
   IDLE ---------------------------> ARMED -------------------------------> BOMB_ACTIVE
    ^  ^                               ^  ^                                  |  |  |  |
    |  |  CANCEL                       |  +------ snooze (max 2) -------------+  |  |  |
    |  +-------------------------------+   CANCEL (also from BOMB_ACTIVE) ------+  |  |
    |                                                                                |  |
    |  30 s                      all modules cleared                                 |  |
    +------- DEFUSED <---------------------------------------------------------------+  |
                                                strikes >= max, or timer 0             |
                         3 s, new bomb (+1 module)                                     v
                  BOMB_ACTIVE <--------------------------------------------------- EXPLODED
```

- `CANCEL` is accepted in ARMED and BOMB_ACTIVE only; it resets the session and pushes no RESULT.
- A **session** starts at the first ARMED -> BOMB_ACTIVE entry and ends when DEFUSED returns to IDLE
  (or on CANCEL). It counts total strikes, snoozes and extra modules.
- `RING_NOW` works in IDLE (-> ARMED) and in ARMED (re-schedules). It needs no time sync.
  `set_alarm` needs a time sync, accepts only IDLE or ARMED, rejects alarms in the past or more than
  7 days ahead, and computes the ms deadline relative to the sync point.

## Bomb composition

Modules = the first `module_count` entries of `module_order`, then one extra module per snooze and
per explosion so far this session, capped at 4 in total. An extra's type is
`(previous module type % 3) + 1` (DIAL -> WIRES -> TRIPWIRE -> DIAL). Each new bomb takes a
snapshot of the config, so `game_set_config` while a bomb runs only affects the next bomb.
The default offline bomb: DIAL, WIRES, TRIPWIRE, 180 s, code 4-2-7-1, 4 wires (correct = index 2),
band 18..22 cm, 3 strikes.

## Config validation (`game_set_config`)

Rejected (previous config kept): `module_count` not 1..4; a used `module_order` entry not 1..3; a
dial digit > 9; `trip_min >= trip_max`; a trip bound outside 5..100; `correct_wire` >= the
clamped `wire_count`. Clamped: `time_limit_s` 30..600, `wire_count` 3..6, `max_strikes` 1..5.

## Module rules

| Module | Input | Effect |
|--------|-------|--------|
| DIAL | `ENC_STEP` | value = (value + step) mod 10 (negatives wrap), CLICK (silent for step 0) |
| DIAL | `ENC_PRESS` | right digit: DIGIT_OK, next digit (value back to 0); 4th right digit clears the module. Wrong: STRIKE, value reset to 0 |
| WIRES | `JOY_LEFT/RIGHT` | cursor moves, clamped (no wrap), CLICK only if it moved |
| WIRES | `JOY_PRESS` | uncut wire: cut it; correct clears the module, wrong = STRIKE (stays cut). Already cut: DENIED |
| TRIPWIRE | `DISTANCE` | inside `[min,max]` starts the hold timer if idle; outside, or > 400, resets it (no strike) |
| any | `TAMPER` | >= 1500 mg in BOMB_ACTIVE = STRIKE, then 2000 ms with tamper ignored |

TRIPWIRE: holding for 3000 ms (checked in `game_tick`) clears the module. No strikes. Inputs for a
module other than the current one are ignored; `TAMPER` counts for any module.

Terminal sounds replace the ordinary one: the fatal strike plays only EXPLODED, the last module
plays only DEFUSED.

## Events

| Event | Value | Effect |
|-------|-------|--------|
| `ENC_STEP` | +1 / -1 (any non-zero works) | dial |
| `ENC_PRESS` | - | dial |
| `ENC_LONG_PRESS` | - | snooze (BOMB_ACTIVE) |
| `JOY_LEFT`, `JOY_RIGHT`, `JOY_PRESS` | - | wires |
| `DISTANCE` | cm | tripwire |
| `TAMPER` | mg | strike if >= 1500 |
| `CMD_RING_NOW` | delay s, clamped 0..3600 | IDLE/ARMED -> ARMED, deadline = now + delay |
| `CMD_SNOOZE` | - | snooze (BOMB_ACTIVE) |
| `CMD_CANCEL` | - | ARMED/BOMB_ACTIVE -> IDLE |

Snooze (BOMB_ACTIVE): if fewer than 2 snoozes, go ARMED with deadline now + 300 s, one more snooze
and one more extra module; otherwise DENIED and nothing changes.

## Timing

- Explosion (strikes >= max or timer reaches 0): push RESULT{EXPLODED, seconds since session start,
  session strikes, snoozes}, play EXPLODED, extras + 1; after 3 s a fresh bomb starts (timer and
  strikes reset, session strike total keeps counting).
- Defuse: push RESULT{DEFUSED, ...}, play DEFUSED; after 30 s back to IDLE and the session is reset.
- In one tick the tripwire hold is checked before the timer, so a hold finishing on the deadline wins.

## Output API

| Function | Meaning |
|----------|---------|
| `game_get_view(g, &view, now)` | `proto_game_state_t` for GAME_STATE |
| `game_view_version(g)` | increments whenever the view changed (as of the last tick/event); send GAME_STATE when it changes |
| `game_tick_interval_ms(g, now)` | **buzzer** tick period: 1000 ms while more than 10 s remain, else `150 + left_ms * 850 / 10000`; 0 = no ticking (all states but BOMB_ACTIVE) |
| `game_led_mode(g)` | OFF (IDLE), ARMED_DIM_BLUE, ACTIVE_RED_BLINK, DEFUSED_GREEN, EXPLODED_RED_WHITE |
| `game_pop_sound(g, &s)` | one-shot FIFO, depth 8, newest dropped when full: CLICK, DIGIT_OK, STRIKE, MODULE_CLEARED, DEFUSED, EXPLODED, DENIED |
| `game_pop_result(g, &r)` | RESULT FIFO, depth 4, newest dropped when full |

### View fields

`state`, `module_idx`, `module_type`, `strikes` (current bomb), `progress_pct` = cleared modules * 100 /
module count, `time_left_s` (BOMB_ACTIVE only, rounded up, 0 elsewhere). Outside
BOMB_ACTIVE/DEFUSED/EXPLODED the module fields and progress are 0.

### `module_value` encoding

| Module | Value |
|--------|-------|
| DIAL | `current_value \| (digit_index << 8)` |
| WIRES | `cursor \| (cut_mask << 8)` (bit n = wire n cut) |
| TRIPWIRE | `min(distance_cm, 255) \| (hold_pct << 8)`, hold_pct 0..100 |

`module_value` is 0 outside BOMB_ACTIVE. The largest value (tripwire, 255 | 100 << 8 = 25855) fits `int16_t`.
