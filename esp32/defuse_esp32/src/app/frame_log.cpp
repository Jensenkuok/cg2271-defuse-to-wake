/* Defuse to Wake - frame log helpers. See frame_log.h. */
#include "frame_log.h"

const char *frame_type_name(uint8_t type)
{
    switch (type) {
    case PROTO_MSG_HEARTBEAT:    return "HEARTBEAT";
    case PROTO_MSG_ACK:          return "ACK";
    case PROTO_MSG_NACK:         return "NACK";
    case PROTO_MSG_TIME_SYNC:    return "TIME_SYNC";
    case PROTO_MSG_SET_ALARM:    return "SET_ALARM";
    case PROTO_MSG_BOMB_CONFIG:  return "BOMB_CONFIG";
    case PROTO_MSG_TAMPER_EVENT: return "TAMPER_EVENT";
    case PROTO_MSG_CMD:          return "CMD";
    case PROTO_MSG_GAME_STATE:   return "GAME_STATE";
    case PROTO_MSG_RESULT:       return "RESULT";
    case PROTO_MSG_TELEMETRY:    return "TELEMETRY";
    default:                     return "?";
    }
}

const char *frame_dir_name(uint8_t dir)
{
    return dir == FLOG_RX ? "RX" : "TX";
}

const char *frame_outcome_name(uint8_t outcome)
{
    switch (outcome) {
    case FLOG_OK:         return "OK";
    case FLOG_ACK:        return "ACK";
    case FLOG_NACK:       return "NACK";
    case FLOG_RETX:       return "RETX";
    case FLOG_CRC_ERR:    return "CRC_ERR";
    case FLOG_SEND_FAIL:  return "SEND_FAIL";
    case FLOG_FAULT_CRC:  return "CRC CORRUPTED (fault)";
    default:              return "?";
    }
}

bool frame_type_is_reliable(uint8_t type)
{
    switch (type) {
    case PROTO_MSG_TIME_SYNC:
    case PROTO_MSG_SET_ALARM:
    case PROTO_MSG_BOMB_CONFIG:
    case PROTO_MSG_TAMPER_EVENT:
    case PROTO_MSG_CMD:
    case PROTO_MSG_RESULT:
        return true;
    default:
        return false;
    }
}

bool frame_type_is_periodic(uint8_t type)
{
    return type == PROTO_MSG_HEARTBEAT || type == PROTO_MSG_GAME_STATE || type == PROTO_MSG_TELEMETRY;
}

FrameLogEntry frame_log_entry(uint32_t ms, uint8_t dir, uint8_t type, uint8_t seq, uint8_t len, uint8_t outcome)
{
    FrameLogEntry e;

    e.ms = ms;
    e.dir = dir;
    e.type = type;
    e.seq = seq;
    e.len = len;
    e.outcome = outcome;
    return e;
}

uint8_t frame_log_seq(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
    if ((type == PROTO_MSG_ACK || type == PROTO_MSG_NACK) && payload != NULL && len >= 1u) {
        return payload[0];
    }
    return seq;
}

uint8_t frame_rx_outcome(uint8_t type)
{
    if (type == PROTO_MSG_ACK) {
        return FLOG_ACK;
    }
    return type == PROTO_MSG_NACK ? FLOG_NACK : FLOG_OK;
}

uint8_t frame_tx_outcome(uint8_t type, bool retransmit, bool corrupted)
{
    if (corrupted) {
        return FLOG_FAULT_CRC;
    }
    return retransmit ? (uint8_t)FLOG_RETX : frame_rx_outcome(type);
}

bool frame_log_record(FrameLogRing *ring, bool include_periodic, const FrameLogEntry &e)
{
    if (ring == NULL) {
        return false;
    }
    if (!include_periodic && e.outcome == FLOG_OK && frame_type_is_periodic(e.type)) {
        return false;
    }
    (void)ring->push(e);
    return true;
}

void frame_corrupt_crc(uint8_t *frame, size_t len)
{
    if (frame != NULL && len > 0u) {
        frame[len - 1u] = (uint8_t)(frame[len - 1u] ^ 0xFFu);
    }
}
