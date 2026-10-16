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
   - Debug console at start-up: `Defuse to Wake MCX: link bring-up (LOOPBACK TEST)`.
   - **GREEN LED** comes on within about 1–2 s and stays on. The link is up because the board
     hears its own 1 s heartbeat.
   - **BLUE LED** toggles every time a frame is delivered: about once a second (heartbeat), plus
     once every 2 s (the TIME_SYNC test message). It flickers irregularly; that is expected.
   - **RED LED** stays off. Solid red means a stack overflow or out-of-heap halt.
   - Every 10 s the console prints something like:

     ```
     [loopback t=10s] tx=25 rx=25 retx=0 fail=0 crc=0 len=0 dup=0 nack=0
       uart_err=0 rxq_ovf=0 ring_drop=0 req_drop=0 delivered: 0x01=10 0x10=5
     ```

     `0x01` = HEARTBEAT, `0x10` = TIME_SYNC. `tx` counts heartbeats, TIME_SYNCs and the ACKs
     the board sends to itself, so `tx` and `rx` keep growing together.

5. **Pass / fail.**
   - PASS if, after at least 60 s:
     - GREEN stays solid.
     - `retx`, `fail`, `crc`, `len`, `uart_err`, `rxq_ovf` and `ring_drop` are all 0.
     - `0x10` grows by 5 every 10 s and `0x01` by about 10 every 10 s.
     - `tx` equals `rx` (±2 for frames in flight when the line is printed).
   - Extra check: **pull the jumper out** for ≥ 5 s. GREEN must go off after about 3 s (link
     down). `retx` and `fail` will rise, because TIME_SYNC frames go unanswered. Put the jumper
     back: GREEN returns within about 1 s.
   - FAIL: GREEN never lights, or any error counter above grows while the jumper is in.
     Report the printed lines.

6. **Switch back.** Set `APP_LOOPBACK_TEST` to `0`, rebuild and flash, and remove the jumper.
   In normal mode GREEN only lights once the ESP32 is connected and talking (PTE22 → ESP32
   GPIO2, PTE23 ← ESP32 GPIO1, grounds tied).
