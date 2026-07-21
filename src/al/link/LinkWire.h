// Wire vocabulary shared across the link: ctrl commands,
// chunk/header sizes, and ACK/NAK wire-byte accounting.
#pragma once
#include <stdint.h>

namespace autolink {

constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t PONG_CMD = 0x33;
constexpr uint8_t REQ_CMD = 0x11;
constexpr uint8_t LOCK_CMD = 0x44;

constexpr int MAX_CHUNK = 250;
constexpr int MSG_HDR = 6;
constexpr int PHASE3_ACKS_NEEDED = 2;
constexpr int RX_ACK_WIRE_BYTES = 8;
constexpr int RX_NAK_WIRE_BYTES = 6;

} // namespace autolink
