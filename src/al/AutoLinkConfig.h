
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "al/link/LinkWire.h"

namespace autolink {

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig;

constexpr int AUTOLINK_ARQ_PIPELINE_WINDOW = 32;

// App-side settle window after a lock. Both ends hold recv() this
// long so neither reads a half-switched baud. Must stay >= the
// library's postLockQuietMs default.
constexpr uint32_t AUTOLINK_APP_SETTLE_MS = 600;

// Wire-side settle window after a lock. The link layer drops
// every incoming frame (no ACK, no NAK, no app-buf write, no
// rxSeq advance) for this long, so a baud switch's line
// garbage and any in-flight frames from the prior session
// cannot trigger an ACK that advances the peer's gbnBase
// against the new session. Distinct from AUTOLINK_APP_SETTLE_MS
// (which gates the *app*'s recv()) and from postLockQuietMs
// (which gates the *sender*'s tx). The wire gate is the small
// sum of: UART FIFO drain (<1ms on real hardware), app-buf
// drain (instant, controlled by the link layer), and one
// cross-iteration slip (worst case ~one RTT). 50ms is
// generous. Anything larger re-introduces the
// wedge (NAK-driven retx during settle fills the peer's GBN
// window with chunks the receiver silently drops, so the
// sender's window stays full until RTO clears it; the field
// log showed the receiver's drain complete 35ms after the
// first spurious NAK — a 50ms gate is 15ms safety over that
// observed figure). Pinned by SettleGateTest.
constexpr uint32_t AUTOLINK_WIRE_SETTLE_MS = 50;

constexpr int chunksForMsgLen(int len) {
    if (len <= 0)
        return 0;

    if (len + MSG_HDR <= MAX_CHUNK)
        return 1;
    int n = (len + MAX_CHUNK - 1) / MAX_CHUNK;
    return 1 + n;
}

// Largest message whose chunk count fits `budget` GBN slots. A
// message longer than the free window can never be admitted while
// any prior chunk is in flight, so an app that draws lengths must
// clamp to this. Pinned by AsyncRandomAdmissionTest.
constexpr int maxLenForChunkBudget(int budget) {
    if (budget <= 0)
        return 0;
    if (budget == 1)
        return MAX_CHUNK - MSG_HDR;
    return (budget - 1) * MAX_CHUNK;
}

// maxLenForChunkBudget against the *live* free window. The GBN
// window, not the ARQ pool, is the binding admission bound (the
// pool is 2*window for retx headroom). Floors at one chunk so the
// app always has a non-zero draw while the pipeline drains. Pinned
// by AsyncRandomAdmissionTest.
constexpr int maxLenForFreeWindow(int window, int inflight) {
    if (window <= 0)
        return MAX_CHUNK - MSG_HDR;
    if (inflight < 0)
        return MAX_CHUNK - MSG_HDR;
    int free_ = window - inflight;
    if (free_ < 1)
        free_ = 1;
    return maxLenForChunkBudget(free_);
}

inline size_t streamBufferFloor(const AutoLinkConfig &cfg);

constexpr size_t AUTOLINK_DEFAULT_MAX_MSG = 5120;

constexpr int COBS_SEQ_SPACE = 254;

constexpr int ARQ_CHUNK_BUDGET = AUTOLINK_ARQ_PIPELINE_WINDOW * 2;
static_assert(ARQ_CHUNK_BUDGET >= 2 * AUTOLINK_ARQ_PIPELINE_WINDOW,
              "ARQ_CHUNK_BUDGET must cover a full window plus retx "
              "headroom; bump the cache POOL_SIZE");
static_assert(chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG) <= COBS_SEQ_SPACE,
              "default maxMsg must fit the seq space alone; "
              "raise COBS_SEQ_SPACE (drop reserved discriminators) or "
              "lower MAX_CHUNK");

static_assert(ARQ_CHUNK_BUDGET >=
                  chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG) * 2,
              "ARQ_CHUNK_BUDGET must hold at least 2x max-chunks-per-msg "
              "so a single large message fits the seq space while at "
              "least one previous message is still in flight");

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = { 512000, 115200, 57600,
                                                  38400,  19200,  9600 };

    int allowedBaudsCount = 6;

    inline int clampToMaxBauds() {
        if (allowedBaudsCount < 0)
            allowedBaudsCount = 0;
        if (allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            allowedBaudsCount = AUTOLINK_MAX_BAUDS;
        return allowedBaudsCount;
    }

    int clampedCount() const {
        if (allowedBaudsCount < 0)
            return 0;
        if (allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            return AUTOLINK_MAX_BAUDS;
        return allowedBaudsCount;
    }

    uint32_t allowedBaudSafe(int i) const {
        if (i < 0 || i >= AUTOLINK_MAX_BAUDS)
            return 0;
        return allowedBauds[i];
    }

    int errThreshold = 100;
    int delayMs = 50;
    size_t rxBufferSize = 2048;
    size_t txBufferSize = 256;
    size_t streamBufferSize = 2048;

    size_t heapReserveBytes = 16384;
    size_t maxMsg = AUTOLINK_DEFAULT_MAX_MSG;
    int ledPin = 2;
    int idleTimeoutMs = 10000;
    int pingSamplesPerBaud = 3;
    float minAcceptRate = 0.5f;
    int errRateWindow = 30;

    enum class Mode : uint8_t { SYNC = 0, ASYNC = 1 };
#ifdef AUTOLINK_HOST_TEST

    Mode mode = Mode::ASYNC;
#else
    Mode mode = Mode::SYNC;
#endif

    int syncAckTimeoutMs = 500;

    // TX admission hold after a re-lock that followed a real link
    // drop; covers the peer's settle + baud-switch window. Escalates
    // with the recent-disc streak so a resweep loop backs itself off.
    int postLockQuietMs = 600;

    uint8_t maxRetx = 5;

    int txDelayMs = 0;

    // Gap between chunks of one multi-chunk ASYNC message, so the
    // burst can't outrun the peer's UART RX-FIFO drain. 0 disables
    // it. No-op in SYNC. Pinned by AsyncChunkGapTest.
    int asyncChunkGapMs = 1;

    // Frames one RTO may resend on a stuck base. Replaying the
    // whole window saturates the wire and starves the peer's ACK
    // path; later RTOs replay the next prefix. <= 0 disables the
    // resend. Pinned by GbnBurstCapTest.
    int gbnResendBurstMax = 8;
};

inline size_t streamBufferFloor(const AutoLinkConfig &cfg) {
    constexpr int kHdr = 6;
    constexpr int multiples = 2;
    size_t floor = (size_t)multiples * (cfg.maxMsg + kHdr);
    return cfg.streamBufferSize > floor ? cfg.streamBufferSize : floor;
}

inline size_t uartRxBufferFloor(const AutoLinkConfig &cfg) {
    constexpr int kFrameOverhead = 4;
    constexpr int kHeadroomMul = 5;
    constexpr int kHeadroomDiv = 4;
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return cfg.rxBufferSize;
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    size_t window = (size_t)AUTOLINK_ARQ_PIPELINE_WINDOW;
    size_t floor = (perChunk * window * kHeadroomMul) / kHeadroomDiv;
    return cfg.rxBufferSize > floor ? cfg.rxBufferSize : floor;
}

inline size_t uartTxBufferFloor(const AutoLinkConfig &cfg) {
    constexpr int kFrameOverhead = 4;
    constexpr int kHeadroomMul = 3;
    constexpr int kHeadroomDiv = 2;
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return cfg.txBufferSize;
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    size_t floor = (perChunk * kHeadroomMul) / kHeadroomDiv;
    return cfg.txBufferSize > floor ? cfg.txBufferSize : floor;
}

inline size_t capFloorByHeap(size_t want, size_t minFloor, size_t freeHeap,
                             size_t reserve) {
    if (minFloor > want)
        minFloor = want;
    if (reserve == 0 || freeHeap == 0)
        return want;
    if (freeHeap >= want + reserve)
        return want;
    size_t avail = freeHeap > reserve ? freeHeap - reserve : 0;
    return avail > minFloor ? avail : minFloor;
}

} // namespace autolink
