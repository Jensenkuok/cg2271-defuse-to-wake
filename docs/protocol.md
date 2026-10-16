# Inter-board protocol (MCXC444 <-> ESP32)

Canonical implementation: `shared/protocol.h` and `shared/protocol.c` (C99, no dynamic
allocation, no globals). `tools/sync_shared.ps1` copies them into the Arduino sketch folder.

**Link:** UART, 115200 baud, 8 data bits, no parity, 1 stop bit (8N1). The codec contains no UART code.

## Frame

```
 byte:   0      1      2      3      4 .. 4+LEN-1    4+LEN
       +------+------+------+------+---------------+------+
       | SOF  | TYPE | SEQ  | LEN  |    PAYLOAD    | CRC8 |
       | 0xAA |  u8  |  u8  | 0-32 |   LEN bytes   |  u8  |
       +------+------+------+------+---------------+------+
                \_____________ CRC covers ____________/
```

| Field   | Size | Notes |
|---------|------|-------|
| SOF     | 1    | Always `0xAA` |
| TYPE    | 1    | Message type (below) |
| SEQ     | 1    | Sequence number, wraps at 255 (usage defined in a later step) |
| LEN     | 1    | Payload length, 0..32; anything larger is a length error |
| PAYLOAD | LEN  | Little-endian fields, explicit byte packing |
| CRC8    | 1    | Over TYPE, SEQ, LEN and PAYLOAD (SOF excluded) |

Maximum frame size: 4 + 32 + 1 = **37 bytes**.

## CRC-8

| Parameter | Value |
|-----------|-------|
| Polynomial | 0x07 |
| Init | 0x00 |
| Reflect in/out | none |
| Final XOR | 0x00 |
| Check value | `CRC("123456789") = 0xF4` |

## Message types

| Type | Value | Direction | Payload | Size |
|------|-------|-----------|---------|------|
| HEARTBEAT | 0x01 | both | `uptime_s` | 4 |
| ACK | 0x02 | both | `acked_seq` | 1 |
| NACK | 0x03 | both | `acked_seq`, `reason` | 2 |
| TIME_SYNC | 0x10 | ESP32 -> MCX | `unix_time` | 4 |
| SET_ALARM | 0x11 | ESP32 -> MCX | `alarm_unix` | 4 |
| BOMB_CONFIG | 0x12 | ESP32 -> MCX | see below | 16 |
| TAMPER_EVENT | 0x13 | ESP32 -> MCX | `magnitude_mg` | 2 |
| CMD | 0x14 | ESP32 -> MCX | `cmd`, `arg` | 3 |
| GAME_STATE | 0x20 | MCX -> ESP32 | see below | 9 |
| RESULT | 0x21 | MCX -> ESP32 | see below | 5 |
| TELEMETRY | 0x22 | MCX -> ESP32 | see below | 8 |

### Payload layouts (offset: type, little-endian)

**HEARTBEAT (4)**: 0: `uptime_s` u32

**ACK (1)**: 0: `acked_seq` u8

**NACK (2)**: 0: `acked_seq` u8; 1: `reason` u8 (1 = CRC, 2 = LEN, 3 = unknown type)

**TIME_SYNC (4)**: 0: `unix_time` u32

**SET_ALARM (4)**: 0: `alarm_unix` u32

**CMD (3)**: 0: `cmd` u8 (1 = RING_NOW, 2 = SNOOZE, 3 = CANCEL); 1: `arg` u16

**BOMB_CONFIG (16)**

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 1 | `module_count` | 1..4 |
| 1 | 4 | `module_order[4]` | 1 = DIAL, 2 = WIRES, 3 = TRIPWIRE |
| 5 | 2 | `time_limit_s` | u16 |
| 7 | 4 | `dial_code[4]` | digits 0-9 |
| 11 | 1 | `wire_count` | |
| 12 | 1 | `correct_wire` | |
| 13 | 1 | `trip_min_cm` | |
| 14 | 1 | `trip_max_cm` | |
| 15 | 1 | `max_strikes` | |

**TAMPER_EVENT (2)**: 0: `magnitude_mg` u16

**GAME_STATE (9)**

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 1 | `state` | 0 IDLE, 1 ARMED, 2 BOMB_ACTIVE, 3 DEFUSED, 4 EXPLODED |
| 1 | 1 | `module_idx` | |
| 2 | 1 | `module_type` | |
| 3 | 2 | `time_left_s` | u16 |
| 5 | 1 | `strikes` | |
| 6 | 1 | `progress_pct` | |
| 7 | 2 | `module_value` | i16 |

**RESULT (5)**: 0: `outcome` u8 (1 = DEFUSED, 2 = EXPLODED); 1: `duration_s` u16; 3: `strikes` u8; 4: `snoozes` u8

**TELEMETRY (8)**: 0: `joy_x` u16; 2: `joy_y` u16; 4: `distance_cm` u16; 6: `encoder_count` i16

`unpack` functions return `false` unless the payload length matches the size above exactly.
Packing never uses `#pragma pack` or struct casts.

## Decoding and resync

The decoder (`proto_decoder_feed`) takes one byte at a time and is safe to call from a
parser task fed by a UART RX queue. States: wait for SOF, TYPE, SEQ, LEN, PAYLOAD, CRC.

- Bytes outside a frame are ignored until `0xAA`. A `0xAA` inside a payload is data, not a new frame.
- `LEN > 32` returns `PROTO_ERR_LEN`; a bad CRC returns `PROTO_ERR_CRC`.
- On either error the decoder discards the failed SOF and replays the bytes it has already
  buffered, starting at the next `0xAA`. A good frame that began inside a truncated or
  corrupted one is therefore still delivered. If that replay completes a frame, the call
  returns `PROTO_FRAME_OK` (the error is still counted).
- Counters `frames_ok`, `crc_errors` and `len_errors` are kept in the decoder.

## Transport

Implemented in `shared/proto_link.{h,c}` on top of the codec above. Platform-agnostic: time is a
`now_ms` argument, bytes leave through a write callback, frames and events leave through callbacks.

### Reliable and unreliable types

| Class | Types | Rule |
|-------|-------|------|
| Reliable (ACKed) | TIME_SYNC, SET_ALARM, BOMB_CONFIG, TAMPER_EVENT, CMD, RESULT | Stop-and-wait, retransmitted until ACKed |
| Unreliable | HEARTBEAT, GAME_STATE, TELEMETRY, ACK, NACK | Written immediately, never ACKed |

Each link has one 8-bit tx SEQ counter, incremented for every new frame (reliable or not).
Retransmits reuse the original SEQ.

### Sender

- One reliable frame is outstanding at a time. Further reliable sends wait in a FIFO of 4 (payload
  copied), so up to 5 reliable frames are in flight or waiting. `proto_link_send` returns false when
  the FIFO is full, the type is unknown, or the arguments are invalid.
- An ACK whose `acked_seq` matches the outstanding frame clears it and sends the next queued frame.
  Other ACKs are ignored.
- Any NACK received while a frame is outstanding retransmits it at once (counts as a retry).
- No ACK within `timeout_ms` of the last transmission also retransmits. After `max_retries`
  retransmits the frame is dropped, `PROTO_EVT_SEND_FAILED(type, seq)` is emitted, and the next
  queued frame is sent.

### Receiver

- A valid reliable frame is always answered with `ACK{acked_seq = SEQ}`. It is delivered to the
  application only if its SEQ differs from the last delivered reliable SEQ from the peer
  (duplicates are re-ACKed but not re-delivered). The first reliable frame is always accepted.
- Decoder CRC error: `NACK{0xFF, 1}`. Decoder length error: `NACK{0xFF, 2}`.
  Valid frame of unknown TYPE: `NACK{its SEQ, 3}`, not delivered.

### Sequence diagrams

```
Normal ACK            Lost frame               Lost ACK (duplicate)       NACK
A         B           A         B              A         B                A         B
|--CMD 5->|           |--CMD 5-X |             |--CMD 5->|                |--CMD 5->| (corrupted)
|         |deliver    |   100 ms |             |         |deliver         |         |CRC error
|<-ACK 5--|           |--CMD 5->| (retx)       |<-ACK 5-X|                |<-NACK---|
|         |           |         |deliver       |   100 ms|                |--CMD 5->| (retx, immediate)
                      |<-ACK 5--|              |--CMD 5->| (retx)         |         |deliver
                                               |         |dup: no deliver |<-ACK 5--|
                                               |<-ACK 5--| (re-ACK)
```

### Constants (`proto_link.h`)

| Constant | Value | Notes |
|----------|-------|-------|
| `PROTO_LINK_DEFAULT_TIMEOUT_MS` | 100 | `cfg.timeout_ms`; 0 = default |
| `PROTO_LINK_DEFAULT_MAX_RETRIES` | 3 | `cfg.max_retries`; 0 = default |
| `PROTO_LINK_HEARTBEAT_MS` | 1000 | `poll` sends `HEARTBEAT{uptime_s = now_ms / 1000}` |
| `PROTO_LINK_DOWN_MS` | 3000 | silence before the link is declared down |
| `PROTO_LINK_TX_DEPTH` | 4 | FIFO behind the outstanding frame |

All time comparisons are wraparound-safe for a 32-bit millisecond counter.

### Link up/down

The link starts down. Any valid frame (including ACK/NACK) sets it up and emits
`PROTO_EVT_LINK_UP` on the transition. If no valid frame arrives for 3000 ms, `proto_link_poll`
sets it down and emits `PROTO_EVT_LINK_DOWN`. Heartbeats keep flowing while down, so the link
recovers by itself.

### Threading rule

A link is not thread-safe by design. One RTOS task owns it and serialises every call to
`proto_link_send`, `proto_link_rx_byte` and `proto_link_poll`. A UART RX ISR only pushes received
bytes into a queue; the owning task pops them and calls `proto_link_rx_byte`. Callbacks run on the
owning task and may call `proto_link_send`, but not `rx_byte` or `poll`.

### Statistics

`link.stats`: `tx_frames` (new frames; retransmits are counted separately), `rx_frames` (valid frames,
duplicates included), `retransmits`, `send_failures`, `crc_errors`, `len_errors`, `dup_drops`,
`nacks_sent`.
