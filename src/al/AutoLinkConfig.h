
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "al/link/LinkFrameSizes.h"

namespace autolink {

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig;

constexpr int AUTOLINK_ARQ_PIPELINE_WINDOW = 32;

// AL-A2: known, fixed static (.bss) contributors this library owns
// regardless of AutoLinkConfig at runtime — the Log singleton
// (dominated by its ESP_PLATFORM-only espRing_ ring, see Log.h) and
// the httpd chunk buffer in AutoLinkWebHandlersData.cpp's
// handleRoot. Measured on host with ESP_PLATFORM defined:
// sizeof(Log) = 23792 B (QUEUE_CAP=128, see Log.h — this already
// includes espRing_, so it is NOT added again below). The static
// `char chunk[1024]` in handleRoot is a separate function-local
// static, not a Log member.
//
// This budget is a partial, host-verifiable lower bound, not a
// substitute for a real link: it covers what this library's own
// source declares as static, not the whole firmware image (Arduino
// core, WiFi/BT buffers, every other library's .bss/.data — see
// AL92-13 vs the real dram0_0_seg overflow this project shipped in
// a prior release, which no host-side number could have caught in
// full).
// What it DOES catch: a future bump to QUEUE_CAP or Entry::msg (or
// a new static/function-local-static buffer anywhere in this
// library) that grows this library's own footprint, on host, before
// any cross-compile is available. See
// test/test_desktop/al/meta/StaticFootprintTest.cpp.
constexpr size_t AUTOLINK_STATIC_DRAM_BUDGET = 32768;

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

    // Heap capFloorByHeap leaves for the rest of the system
    // (LWIP, httpd, WiFi). The default 24576 B is sized for
    // a 41 KB post-WiFi free heap with the web monitor's
    // maxMsg=2048 cap installed (residual 25188 B). A device
    // with more free heap can raise this; a tight-heap
    // device can lower it to 16384 (the prior-release
    // default), but then the 20 KB serviceable floor in
    // EspHal::begin() will fire on the field device and
    // abort. The remediation: pair a lower reserve with a
    // lower maxMsg (AutoLinkWeb's ctor caps it to 2048 by
    // default; AutoLink::setMaxMsg(N) for non-web users).
    // Pinned by EspHalHeapAccountingTest's field-numbers
    // case.
    size_t heapReserveBytes = 24576;
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
    // AL89-2: SYNC floor sized to msgChunks + 2. The
    // multi-chunk path now reserves a single chunk
    // (AL89-1 removed the whole-burst pre-drain), so
    // the ring only needs headroom for the in-flight
    // data chunk + a concurrent outbound ACK + the
    // per-message header overhead. Without the +1 of
    // headroom a fully-loaded ring that just received
    // an in-flight chunk has no room for the peer ACK
    // it has to send in the same window. Pinned by
    // SyncFloorHasAckHeadroomTest.
    if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
        int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
        if (msgChunks < 1)
            msgChunks = 1;
        size_t syncFloor =
            (size_t)kWorstCaseCobsFrame * (size_t)(msgChunks + 2);
        return cfg.txBufferSize > syncFloor ? cfg.txBufferSize : syncFloor;
    }
    // ASYNC needs headroom for the retx burst AND a
    // full-message multi-chunk send (chunksForMsgLen).
    // Earlier shape sized the floor to the retx
    // budget only; a 22-frame 5.6 KB maxMsg message
    // at 512000 then blocked uart_write_bytes for
    // ~107 ms, starving the peer's onRx / onTimer.
    // Floor = max(retx budget, full-message chunk set)
    // so neither path wedges the link lock. Pinned by
    // UartTxBufferRetxBurstTest +
    // SendMsgTxAvailBoundTest.
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    int burst = cfg.gbnResendBurstMax > 0 ? cfg.gbnResendBurstMax : 0;
    size_t retxFloor = perChunk * (size_t)(burst + 1);
    int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
    if (msgChunks < 1)
        msgChunks = 1;
    size_t msgFloor = perChunk * (size_t)msgChunks;
    // AL88-4: without a window-sized floor, a ring provisioned for
    // only one maxMsg burst forces Link::begin()'s installed-ring
    // clamp (LinkCore.cpp) down to ~1 message's worth of chunks —
    // the GBN window collapses to effectively SYNC-like stop-and-
    // wait even though the app is admitting for a full pipeline.
    // Field measurement: window clamped to 10 against a compile-time
    // 32, ASYNC throughput ~3% of SYNC's on the same wire. Size the
    // floor for a full pipeline window so LinkCore's clamp is a
    // safety net for genuinely heap-starved rings, not the normal
    // case. Pinned by AsyncRingSizedForPipelineWindowTest.
    size_t windowFloor = perChunk * (size_t)AUTOLINK_ARQ_PIPELINE_WINDOW;
    size_t floor = retxFloor > msgFloor ? retxFloor : msgFloor;
    if (windowFloor > floor)
        floor = windowFloor;
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
    // J1: if the heap-stripped headroom can't afford the minFloor, return 0 —
    // the caller (Link::begin()'s gate) needs to see the shortfall to fail
    // loudly. Pinned by BeginRejectsHeapClampedRingTest (a 9000-B free heap
    // with a 16384-B reserve must return 0, not 262).
    if (avail < minFloor)
        return 0;
    return avail;
}

// D14: apply the heap-cap before the floor
// comparison. The earlier shape returned
// `cfg.txBufferSize > floor ? cfg.txBufferSize :
// floor` — a 4 KB txBufferSize with an 8 KB floor
// would be *clamped up* to 8 KB even on a board
// with 32 KB free heap and a 4 KB reserve. The
// allocation would then split the heap and the
// next allocator hit (a wifi rx mbuf) would push
// the high-water mark above the threshold.
//
// E5: the previous `minFloorCapped` shape was
// both broken and self-defeating:
//   - the inner `min(minFloor, cfg.txBufferSize)`
//     collapsed a 5.5 KB floor to 256 B when
//     txBufferSize defaulted to 256, and
//   - the helper's `minFloor > want` clamp
//     then made 256 B the *maximum* the helper
//     could return.
//
// The right contract: the user-set
// `cfg.txBufferSize` is the *minimum they want*,
// not the *maximum*. The floor (derived from
// retx budget + maxMsg chunks) is the actual
// minimum needed for the link to function —
// take max(txBufferSize, floor) as the want,
// then cap by heap. Pinned by
// UartTxBufferHeapCapTest (a 256 B
// txBufferSize with a 5.5 KB floor and a 32 KB
// free-heap reads back as 5.5 KB — the floor
// is honoured, the user want is the lower
// bound, the heap is the upper bound).
inline size_t uartTxBufferFloorCapped(const AutoLinkConfig &cfg,
                                      size_t freeHeap, size_t reserve) {
    size_t userWant = cfg.txBufferSize;
    size_t floor = uartTxBufferFloor(cfg);
    // Floor must be at least the user's
    // requested txBufferSize; the floor
    // function already does this internally
    // but the contract here is explicit. If
    // the user wants more than the floor
    // (rare — they configured a high
    // txBufferSize for headroom), honour it.
    size_t want = userWant > floor ? userWant : floor;
    // I2: the TX path is the only floor that passed minFloor = 0 to
    // capFloorByHeap. The kWorstCaseCobsFrame minimum guarantees that any
    // ring the heap-cap allows still fits one frame — Link::begin() (I1)
    // reads the installed size via hw.txRingSize() and rejects anything
    // below. Pinned by BeginRejectsHeapClampedRingTest.
    return capFloorByHeap(want, (size_t)kWorstCaseCobsFrame, freeHeap, reserve);
}

// Distributes cfg.heapReserveBytes once against totalFreeHeap, then hands
// each of streamBuf/rxBuf/txBuf as much of what's left as it wants, in
// that order, falling to 0 for a buffer only when even its own floor
// can't be met from the remaining budget. This replaces calling
// capFloorByHeap separately for each buffer with the SAME reserve
// against an already-shrunk running total: that shape re-charges the
// reserve at every step, so once any earlier buffer clamps, the running
// total converges to exactly `reserve` and whichever buffer is sized
// last computes avail = reserve - reserve = 0 regardless of how much
// heap is genuinely still free. EspHal::begin() and
// EspHalHeapAccountingTest both call this — a single source of truth
// instead of the test hand-replicating begin()'s arithmetic (which is
// how the earlier version of this bug went uncaught: the test asserted
// against a reimplementation, not the code that actually ran).
struct HeapDistribution {
    size_t streamBuf;
    size_t rxBuf;
    size_t txBuf;
    size_t postFree;
};

inline HeapDistribution distributeHeapBudget(const AutoLinkConfig &cfg,
                                             size_t totalFreeHeap) {
    size_t budget = totalFreeHeap > cfg.heapReserveBytes
        ? totalFreeHeap - cfg.heapReserveBytes
        : 0;
    auto take = [&budget](size_t want, size_t minFloor) -> size_t {
        if (minFloor > want)
            minFloor = want;
        if (want <= budget) {
            budget -= want;
            return want;
        }
        if (budget >= minFloor) {
            size_t got = budget;
            budget = 0;
            return got;
        }
        return 0;
    };
    HeapDistribution d{};
    size_t sbMin = (size_t)cfg.maxMsg + 6;
    d.streamBuf = take(streamBufferFloor(cfg), sbMin);
    d.rxBuf = take(uartRxBufferFloor(cfg), cfg.rxBufferSize);
    size_t txFloor = uartTxBufferFloor(cfg);
    size_t txWant = cfg.txBufferSize > txFloor ? cfg.txBufferSize : txFloor;
    d.txBuf = take(txWant, (size_t)kWorstCaseCobsFrame);
    size_t used = d.streamBuf + d.rxBuf + d.txBuf;
    d.postFree = totalFreeHeap > used ? totalFreeHeap - used : 0;
    return d;
}

} // namespace autolink
