# Hardware test: MCXC444 UART2 link loopback

Goal: prove that the interrupt-driven UART2 driver and the ACK/retransmit link layer work on the
real board, with no ESP32. With PTE22 (TX) jumpered to PTE23 (RX), the board receives its own
frames, so it ACKs its own reliable messages.
Rubric lines: Comms protocol (packetization + flow control), ISR design, Task design.

## Steps

1. **Import and flash.** Import `mcxc444/defuse_mcx` as described in `mcxc444/README.md`
   (Existing Projects, do NOT copy into the workspace). Build: expect `0 errors, 0 warnings`.
   Open a serial terminal on the MCU-Link VCOM port at 115200 8N1.

2. **Jumper.** Unplug USB. Connect **J4 pin 5 (PTE22, UART2 TX)** to **J4 pin 7 (PTE23,
   UART2 RX)** with one jumper wire (docs/pin_table.md). Nothing else is connected.

3. **Enable the self-test.** In `source/app_config.h` set

   ```c
   #define APP_LOOPBACK_TEST           1
   ```

   Rebuild and flash (Debug, then Resume/F8).

4. **What you should see.**
   - Debug console at start-up: `Defuse to Wake MCX [LOOPBACK TEST]`.
   - LED (LED mode OFF): a short **blue blink every 2 s while the link is DOWN**. It stops
     within about 1–2 s, because the board hears its own 1 s heartbeat and the link comes up.
     A dark LED means the link is up. (The feedback task owns the RGB LED; the earlier
     "green = link up" behaviour was replaced by this blink.)
   - **RED LED** stays off. Solid red means a stack overflow or out-of-heap halt.
   - Every 10 s the console prints something like:

     ```
     [loopback t=10s] tx=25 rx=25 retx=0 fail=0 crc=0 len=0 dup=0 nack=0
       uart_err=0 rxq_ovf=0 ring_drop=0 req_drop=0 delivered=15: 0x01=10 0x10=5
     ```

     `0x01` = HEARTBEAT, `0x10` = TIME_SYNC. `tx` counts heartbeats, TIME_SYNCs and the ACKs
     the board sends to itself, so `tx` and `rx` keep growing together.

5. **Pass / fail.**
   - PASS if, after at least 60 s:
     - No blue blink (link stays up).
     - `retx`, `fail`, `crc`, `len`, `uart_err`, `rxq_ovf` and `ring_drop` are all 0.
     - `0x10` grows by 5 every 10 s and `0x01` by about 10 every 10 s.
     - `tx` equals `rx` (±2 for frames in flight when the line is printed).
   - Extra check: **pull the jumper out** for ≥ 5 s. the blue blink must start after about 3 s (link
     down). `retx` and `fail` will rise, because TIME_SYNC frames go unanswered. Put the jumper
     back: the blink stops within about 1 s.
   - FAIL: the blue blink never stops, or any error counter above grows while the jumper is in.
     Report the printed lines.

6. **Switch back.** Set `APP_LOOPBACK_TEST` to `0`, rebuild and flash, and remove the jumper.
   In normal mode the blue link-down blink only stops once the ESP32 is connected and talking (PTE22 → ESP32
   GPIO2, PTE23 ← ESP32 GPIO1, grounds tied).
