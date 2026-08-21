// Message-fill helpers split out of Ping.h to keep
// the per-file 15 KB cap. The patterns are the
// same ones Ping::loop() uses to draw a per-tick
// payload: clamp the draw to the LIVE free window
// (not just the static half-window ceiling), fill
// the buffer with a deterministic pattern for
// SEQUENTIAL, random bytes for RANDOM. The free
// function wrappers here are inline so the
// header-only Ping library keeps zero-cost
// abstraction.
#pragma once
#include <stdint.h>
#include "al/pingpong/PingPongBase.h"

namespace autolink {

// Clamp a random-draw size to the live free window
// so a draw sized above the free slot count is
// rejected by sendMsg. Without this, a stuck base
// spins whole-window retransmits until the link
// drops. Pinned by AsyncRandomAdmissionTest.
inline int pingPickMsgSizeClamped(int maxSeqSize, int liveCap,
                                  int randomMinBytes, int randomMaxBytes) {
    int cap = maxSeqSize;
    if (randomMaxBytes < cap)
        cap = randomMaxBytes;
    if (liveCap < cap)
        cap = liveCap;
    int minSize = randomMinBytes;
    if (minSize > cap)
        minSize = cap;
    int span = cap - minSize + 1;
    if (span < 1)
        span = 1;
    return minSize + (int)random((uint32_t)span);
}

inline void pingFillSequential(uint8_t *b, int n) {
    static const char HEX_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t)HEX_DIGITS[i % 36];
}

inline void pingFillRandom(uint8_t *b, int n) {
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t)random(256);
}

} // namespace autolink
