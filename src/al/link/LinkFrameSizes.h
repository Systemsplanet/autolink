// Frame geometry only. Split from LinkWire.h so AutoLinkConfig.h — the
// lowest layer, included by the HAL, the facade and the examples — can
// size its buffer floors without pulling in the link-layer wire
// vocabulary. Keep this a leaf: no includes beyond <stdint.h>.
#pragma once
#include <stdint.h>

namespace autolink {

constexpr int MAX_CHUNK = 250;
constexpr int MSG_HDR = 6;
// 1 (preamble) + rawLen + ceil(rawLen/254) (COBS worst-case expansion)
// + 1 (delim) + 1 (seq) + 1 (CRC8). The naive MAX_CHUNK + 4 bound is
// short of this and lets uart_write_bytes block on a near-full ring.
// Pinned by SendMsgTxAvailBoundTest.
constexpr int kWorstCaseCobsFrame = 1 + (MAX_CHUNK + MSG_HDR) +
    (((MAX_CHUNK + MSG_HDR) + 254 - 1) / 254) + 1 + 1 + 1;

} // namespace autolink
