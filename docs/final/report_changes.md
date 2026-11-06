# Final report: changes since the version sent to the group

The current list lives in the shared Claude Doc "B02_10 Final report: changes to make"
(https://claude.ai/artifact/9Mkxm45j5P94bGT1zncw5W), checked against the SharePoint report of 6 Nov 2026
and code fb5595e. Keep that doc updated; this file is the older working copy (code state d1d3689).

## Must change (the report is now wrong)

1. **Feature list, lead time.** "a lead time of 15 to 180 minutes (default 60)"
   → "1 to 180 minutes (default 60), typed or set with a slider".

2. **Feature list, module order.** "Four sensor modules in fixed order (dial, wires, tripwire, lights)"
   → "All four sensor modules (dial, wires, tripwire, lights) in every bomb, in a random order per bomb".

3. **"The ESP32 BOOT button and the MCX switch SW3 trigger a demo ring after 3 s."** Keep this. Add the
   demo morning alarm: the web page sets an alarm 1 to 30 minutes ahead. It travels the real path
   (SET_ALARM, ARMED, ring), so it shows the same flow as a lecture morning.

4. **LLM paragraph, puzzles (rewrite).** The LLM no longer makes puzzles. Replace the puzzle part of the
   LLM paragraph (the setter/checker pipeline and "A run makes 4 dial and 4 wire puzzles ...") with:
   - Every dial and wire puzzle comes from the ESP32's own generator: it picks the answer, builds the
     clues, proves by brute force (all 10,000 codes / every wire) that there is exactly one solution, and
     rates the difficulty. It works offline and never waits for the network.
   - The device keeps 4 dial + 4 wire puzzles. A pair is used up when its bomb starts, never shown again,
     and refilled at once from the generator (also during a bomb). Only if all four pairs are used before
     a refill is the oldest pair reused.
   - The LLM writes only the morning briefing and coach line (item 5).
   - The setter/checker code is still in the source behind the flag APP_PUZZLE_USE_LLM (0); see the new
     Challenges entry 9 for why it was switched off.
   - LLM limits: 20 s per call, 3000-token reply budget; the briefing chain is qwen3.6:35b, x-test-1, default.

5. **LLM paragraph, briefing.** Add: "bad replies become templates, and the ESP32 retries every 60 s
   while online until the LLM has written the briefing and coach message for the next class day. It
   also runs once after every boot. A demo alarm defers it."

5b. **Protocol paragraph.** Add BOMB_REPORT (MCX to ESP32, reliable) and CMD_REPORT, the cfg_id byte in
   BOMB_CONFIG (17 bytes), and the separate sequence counter for reliable frames (entry 11b). Host test counts
   in item 6 have grown (codec 133, link 714, game 382, plus cfg_sync 55); re-run tools/run_tests.ps1 and copy
   the summary before submitting.

6. **Host tests.** "15 test programs, all passing when run on 28 Oct 2026, including the codec (91
   checks), link layer (148), game engine (353), scheduler (209), rule language (6237) and procedural
   bank (39573)"
   → "15 test programs, about 2.77 million checks, all passing on 6 Nov 2026: codec 91, link layer 148,
   game engine 353, scheduler 227, history 46, puzzle logic 1,181,714, difficulty 1,540,792, text 188,
   web logic 216, frame log 110, rule language 6237, gate 122, procedural bank 39573, light hold 118,
   web page 76".
   Also change "The scheduler (209 host checks)" in the NUSMods paragraph to 227.

7. **Local web page paragraph.** "The main page (next alarm, NUSMods link, lead-time slider, difficulty
   mode, Save, Ring now) polls every 2 s." Rewrite the list:
   - next-alarm countdown, with a "demo" badge when a demo alarm is set;
   - NUSMods link, and the timetable as a week grid (from /api/timetable);
   - lead time typed or by slider (1 to 180 min);
   - difficulty mode;
   - a "This morning" card with the briefing and coach text;
   - demo alarm control (1 to 30 min);
   - Save;
   - Ring now (asks to confirm).
   On the /eng puzzle panel, mention the used-pair mask and that it shows "LLM puzzles off: built-in bank".

8. **State management / session guard paragraph.** Add: a demo alarm is refused during a bomb session.
   If the real alarm comes earlier, the real one wins. The demo never costs the user the real alarm.

## New Challenges entries (short, optional but they earn credit)

9. **LLM puzzles were unreliable, so we stopped using them (optional, strong entry).** First the LLM
   invented the puzzle logic: most Hard puzzles had several solutions, none, or rules that did not parse. We
   moved the logic to the ESP32 and let the LLM only reword it, with a second model solving the riddle blind.
   Board tests then showed the checker was the weak point: one model timed out at 20 s every call, and the
   fast one "solved" Hard riddles wrong (1829 for 4750). Lesson: LLMs reword well but cannot be trusted to do
   logic search. Puzzles now come only from the ESP32's proven generator, and the LLM does what it is good at:
   the morning briefing and coach line. The pipeline stays behind a compile flag.

9b. **Demo alarm without losing the real one.** Both share one SET_ALARM slot on the MCX. The ESP32 keeps
   the real alarm and sends whichever is earlier; the demo is dropped as soon as the real one is
   earlier.

10. **Random order vs the dial code.** The MCX shifts a repeated dial's code by its module index. Once
    modules were shuffled, the shown puzzle's code was only right when the dial came first. The ESP32 now
    sends a pre-shifted code, so the MCX expects the shown puzzle's answer wherever the dial lands. No MCX
    change was needed.

11. **Never repeat a puzzle, never wait for one.** The pair for the next bomb is sent ahead of time, so
    it can't be changed during a session. If it is replaced outside a session, it is resent whole, so
    the OLED text and the MCX's answer always come from the same pair.

11b. **The screen and the game must agree (strong entry, Comms protocol).** On the board the OLED once showed
    "cut the 3rd wire" while the MCX wanted wire 2. Cause: the ESP32 took the link-layer ACK as "the MCX now
    uses this puzzle", but an ACK only proves the bytes arrived; the game may still reject the config, and an
    ESP32 reset forgot which puzzle the MCX held. Fix: an application-level confirmation. Every BOMB_CONFIG
    carries a cfg_id; the MCX answers with a reliable BOMB_REPORT (HELD after each config, RUNNING when a bomb
    starts, both on request at every link-up), and the ESP32 draws the puzzle only from those reports, matched
    by id and bytes; an unknown one gets a built-in clue made from the MCX's real answer. A second bug found on
    the way: reliable frames shared their sequence numbers with heartbeats, so about 1 in 256 was ACKed but
    dropped as a duplicate; reliable frames now have their own counter.

## Still true, keep as is

- "A refresh runs at boot, each evening after 22:00 ..." in the NUSMods paragraph. That is the
  timetable refresh, which still runs at 22:00. Only the puzzle run was dropped.

## Optional

12. Requirements checklist, ESP32 row: list the JSON endpoints (/api/status, /api/timetable,
    /api/settings, /api/demo_alarm, /api/ring, /api/fault/*, /api/puzzles).
13. Note in the history paragraph that demo and Ring-now bombs are recorded like real mornings (they
    feed the Auto difficulty and the coach).
