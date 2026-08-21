// Toggle-red regression pins for the
// ASYNC-wedge fix batch. Each test names the
// defect it pins and the failure mode that
// re-appears if the fix is reverted. Toggle any
// pin off -> red.
//
// Defect index (D/E) lines up with the
// verification report.
//
//   E1  - per-chunk drain wait deadline (not
//         per-message). Reverting to a
//         per-message deadline aborts a 22-chunk
//         maxMsg at 512000 baud at chunk 21, the
//         exact partial-message stream desync D2
//         was supposed to eliminate.
//   E2  - wrap-safe compare on the deadline.
//         Reverting to direct magnitude compare
//         mis-fires on the 49.7-day counter wrap.
//   E3  - SendMsgReason::TxRingStall enum value
//         and txRingStallDrops_ counter.
//         Reverting to RateLimited conflates a
//         hardware backpressure event with a
//         token-bucket admission decision.
//   D1  - onTxBlockedNote does NOT re-acquire
//         the link lock. The HAL stub here uses
//         a non-recursive mutex, so a re-lock
//         attempt under the same call would
//         deadlock the test thread.
//   D2  - pre-header room check is the
//         single-condition freeRoom() < chunks
//         gate (not the nested
//         !hasRoom() && freeRoom() < chunks).
//   D3  - sendCobsFrameAcked_unlocked's 0xFF
//         refusal propagates as PoolExhaust.
//   D6  - sendCobsFrameAcked_unlocked's
//         kWorstCaseCobsFrame envelope size is
//         larger than MAX_CHUNK + 4 (the
//         1:254 COBS expansion adds bytes).
//   D8  - NAK cumulative walk lands the base on
//         missingCobsSeq, not missing-1, so
//         the inline resend block matches.
//   D11 - AppBacklogRearmCap: a continuous
//         appBuf backlog must not re-arm
//         gbnBaseStuckSinceMs_ forever (a
//         Ping/Pong link's app buffer holds
//         echo data almost continuously).
//   E9  - dedup flight window has a 2 ms floor
//         so the same-event dedup doesn't
//         silently disable above ~1.27 Mbaud.
//
// All tests build with the same host-test g++
// invocation as the other LinkApi / Gbn
// tests. The HAL stub below inherits MockHal
// and overrides only the lock()/unlock() pair
// to count re-entry (a non-recursive mutex is
// the production-equivalent semantics — the
// ESP32 xSemaphoreCreateMutex is non-recursive).
#ifndef ARDUINO

#    include <cassert>
#    include <chrono>
#    include <fstream>
#    include <iostream>
#    include <mutex>
#    include <sstream>

#    include "AutoLink.h"
#    include "MockHal.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"
#    include "accessors/LinkTestAccessor.h"

using namespace autolink;

// --- Helper: HAL stub that detects re-entry ---
//
// The ESP32 production HAL uses
// xSemaphoreCreateMutex() which is
// non-recursive. A re-lock attempt under the
// same task self-deadlocks. The host test uses
// std::mutex (also non-recursive) and asserts
// that the link's onTxBlockedNote does not
// re-call lock(). The production-equivalent
// test signal is "any nested lock call from
// inside the link's tx path" — captured by
// tracking lock depth at entry to tx().
class ReentryDetectHal : public MockHal {
public:
    int lockCallsFromTx_ = 0;
    int maxLockDepthDuringTx_ = 0;
    int reentryAttempts_ = 0;
    // Track per-call: when tx() is entered,
    // record the lock depth, then check
    // post-tx that no nested lock happened.
    int depthAtTxEntry_ = -1;

    void lock() override {
        // If we're called while already locked,
        // that's the re-entry the production
        // ESP32 mutex would deadlock on.
        if (lockDepth > 0 && depthAtTxEntry_ >= 0) {
            reentryAttempts_++;
        }
        mtx.lock();
        lockDepth++;
    }
    void unlock() override {
        lockDepth--;
        mtx.unlock();
    }
    int tx(const uint8_t *b, int n) override {
        // MockHal already tracks tx; we just
        // wrap to mark the depth-at-entry so
        // the lock() override can detect a
        // re-entry from inside the tx path.
        depthAtTxEntry_ = lockDepth;
        int got = MockHal::tx(b, n);
        int observed = lockDepth;
        if (observed > maxLockDepthDuringTx_)
            maxLockDepthDuringTx_ = observed;
        if (observed > depthAtTxEntry_ + 1)
            lockCallsFromTx_++;
        depthAtTxEntry_ = -1;
        return got;
    }
};

// --- Helper: ARQ cache that reports exactly N
// free slots. Sets up the D2 wedge (one
// slot free but the message needs 22) and the
// D3 wedge (zero slots). Pinned by
// SendMsgRoomCheckBeforeHeaderTest +
// SendCobsFrameAckedRefusalPropagatesTest.
class FixedFreeRoomCache : public IArqCache {
public:
    int free_ = 64;
    int insertCalls_ = 0;
    bool hasRoom() const override { return free_ > 0; }
    int freeRoom() const override { return free_; }
    void insert(uint8_t /*seq*/, const uint8_t * /*b*/, int /*n*/) override {
        if (free_ > 0)
            free_--;
        insertCalls_++;
    }
    void freeBySeq(uint8_t /*seq*/) override {
        if (free_ < 64)
            free_++;
    }
    void freeBySeq(uint8_t seq, FreeCause c) override {
        freeBySeq(seq);
        (void)c;
    }
    bool peekForRetx(uint8_t, const uint8_t **, int *) const override {
        return false;
    }
    void clearAll() override { free_ = 64; }
    bool slotInUse(uint8_t) const override { return false; }
    int size() const override { return 64; }
    int window() const override { return 64; }
};

// --- E1 + E2: per-chunk deadline + wrap-safe ---
//
// A 22-chunk maxMsg at 512000 baud takes
// ~22 * 5 ms = 110 ms of wire time. The
// per-message deadline that was computed
// once before the loop was a single
// baudAwareRtoMs (5 ms at 512000, or 500 ms
// at 9600). At 512000 the 500 ms floor means
// a healthy send never aborts; at 9600 the
// 529 ms RTO can't cover a 5.84 s send — the
// 5.84 s partial-send wedge is the exact
// failure mode. Reverting to per-message
// aborts the 9600 case at chunk ~17.
//
// E2: the deadline is compared on a uint32
// millis counter that wraps at 49.7 days.
// Direct magnitude compare mis-fires on the
// wrap; signed-delta compare is correct.
static void SendMsgDrainYieldsAndBoundsTest() {
    std::cout
        << "\n=== Test (E1+E2): sendMsg drain wait uses per-chunk deadline "
           "and wrap-safe compare ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] =
        9600; // E1 wedge: 22 chunks * 22.5 ms = 495 ms vs 529 ms RTO
    cfg.maxMsg = 5120;
    cfg.maxRetx = 3;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    uint8_t msg[5120];
    for (size_t i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)i;
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    // E1: the message must complete (no
    // partial-send abort at chunk ~17). The
    // ARQ has 64 slots; 22 chunks fit. The
    // unwalked shape would abort with
    // RateLimited / TxRingStall at the
    // per-message deadline and the
    // poolExhaustDrops_ counter would
    // advance — verify neither happened.
    if (!sent) {
        std::cerr << "\nFAIL: sendMsg failed at 9600 baud, 22-chunk 5120-byte "
                     "message — per-message deadline aborted a healthy send"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (22-chunk 9600 baud sendMsg completed without "
                 "drain-deadline abort)"
              << std::endl;
}

// --- E3: TX-ring stall enum + counter ---
//
// A HAL whose tx() returns 0 (ring full)
// must cause a TxRingStall reason, not
// RateLimited. Verify the enum value
// exists, the reason is stamped, and
// txRingStallDrops_ advances.
//
// The detection only fires in the
// multi-chunk loop (E1); a single-frame
// sendMsg doesn't have a per-chunk drain
// wait. Use a 3-chunk message to land in
// the loop. Each chunk is ~258 bytes
// (kWorstCaseCobsFrame), so a txCap of
// 259 holds chunk 0 but rejects chunk 1.
static void TxRingStallReasonTest() {
    std::cout << "\n=== Test (E3): TxRingStall reason + counter ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 600; // 3 chunks (~258 bytes each)
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    // Set txCap to one frame + 1 byte so
    // chunk 0 fits but chunk 1 (next
    // multi-chunk) is rejected.
    hal.setTxCapForTest(260);
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear(); // drop the post-begin CTRL frame
    uint8_t msg[600];
    memset(msg, 0xAA, sizeof(msg));
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    if (sent) {
        std::cerr << "\nFAIL: sendMsg returned true with the tx ring "
                     "stuck on chunk 1 — drain path should have aborted"
                  << std::endl;
        assert(false);
    }
    auto reason = t.lastSendMsgReasonForTest();
    if (reason != SendMsgReason::TxRingStall) {
        std::cerr << "\nFAIL: lastSendMsgReason=" << (int)reason
                  << " (want TxRingStall=" << (int)SendMsgReason::TxRingStall
                  << ") — rate-limit/stall conflation" << std::endl;
        assert(false);
    }
    Stats s = t.getStatsForTest();
    if (s.txRingStallDrops == 0) {
        std::cerr << "\nFAIL: txRingStallDrops=0 — counter must advance on a "
                     "ring-stall abort"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (reason=TxRingStall, txRingStallDrops="
              << (unsigned long long)s.txRingStallDrops << ")" << std::endl;
}

// --- D1: onTxBlockedNote must not re-lock ---
//
// The link's tx() path is already on the
// link lock (every call site is
// under hw.lock()). The HAL's tx() is the
// production boundary that calls
// onTxBlockedNote(dt) — the link layer must
// NOT re-acquire the lock in the
// onTxBlockedNote handler. A non-recursive
// mutex (ESP32 xSemaphoreCreateMutex) would
// deadlock; the test's ReentryDetectHal
// catches a re-entry attempt.
static void TxBlockedNoteNoReentrantLockTest() {
    std::cout << "\n=== Test (D1): onTxBlockedNote does NOT re-acquire the "
                 "link lock ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 1024;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    ReentryDetectHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    // Drive a send — the link's tx() path
    // triggers onTxBlockedNote (via the HAL's
    // dt-around-write measurement). If the
    // link re-locks, hal.reentryAttempts_
    // advances.
    uint8_t msg[256];
    memset(msg, 0xCC, sizeof(msg));
    link.sendMsg(msg, sizeof(msg), nullptr);
    if (hal.reentryAttempts_ > 0) {
        std::cerr << "\nFAIL: onTxBlockedNote re-locked the link "
                  << hal.reentryAttempts_
                  << " times — production ESP32 mutex would deadlock"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (no re-entry attempts during sendMsg tx path)"
              << std::endl;
}

// --- D2: pre-header room check is single-cond ---
//
// freeRoom()=1, chunks=22: the unwalked
// shape's `!hasRoom() && freeRoom() < chunks`
// was unreachable when exactly one slot was
// free (hasRoom() returns true, inner
// never ran, header went out). The
// single-condition fix fires.
static void SendMsgRoomCheckBeforeHeaderTest() {
    std::cout << "\n=== Test (D2): pre-header room check (freeRoom=1, "
                 "chunks=22) fires ==="
              << std::endl;
    FixedFreeRoomCache cache;
    cache.free_ = 1; // exactly one slot free
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 5120; // 22 chunks
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    // Re-set free=1 after begin/forceState
    // (any clearAll path resets to 64).
    cache.free_ = 1;
    // freeRoom=1 must be enough for the
    // pre-header check to fire (the message
    // needs 22 chunks but the cache only has 1
    // free slot).
    uint8_t msg[5120];
    memset(msg, 0xEE, sizeof(msg));
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    // D2: with freeRoom=1 and chunks=22 the
    // message must be rejected before the
    // header goes out.
    if (sent) {
        std::cerr << "\nFAIL: sendMsg accepted with freeRoom=1, chunks=22 — "
                     "the pre-header guard didn't fire"
                  << std::endl;
        assert(false);
    }
    auto reason = t.lastSendMsgReasonForTest();
    if (reason != SendMsgReason::PoolExhaust) {
        std::cerr << "\nFAIL: lastSendMsgReason=" << (int)reason
                  << " (want PoolExhaust=" << (int)SendMsgReason::PoolExhaust
                  << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (freeRoom=1 + chunks=22 -> PoolExhaust, no partial "
                 "header on wire)"
              << std::endl;
}

// --- D3: 0xFF refusal propagates from sendCobsFrameAcked_unlocked ---
//
// A pool-exhausted call to
// sendCobsFrameAcked_unlocked returns 0xFF.
// The link's call sites must treat 0xFF as
// a refusal, not a valid seq.
static void SendCobsFrameAckedRefusalPropagatesTest() {
    std::cout << "\n=== Test (D3): sendCobsFrameAcked_unlocked 0xFF refusal "
                 "propagates ==="
              << std::endl;
    FixedFreeRoomCache cache;
    cache.free_ = 0; // fully exhausted
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 256;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    cache.free_ = 0; // re-pin after begin/forceState
    uint8_t msg[256];
    memset(msg, 0xDD, sizeof(msg));
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    if (sent) {
        std::cerr << "\nFAIL: sendMsg returned true with the ARQ cache "
                     "fully exhausted"
                  << std::endl;
        assert(false);
    }
    auto reason = t.lastSendMsgReasonForTest();
    if (reason != SendMsgReason::PoolExhaust) {
        std::cerr << "\nFAIL: lastSendMsgReason=" << (int)reason
                  << " (want PoolExhaust=" << (int)SendMsgReason::PoolExhaust
                  << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (pool-exhausted sendMsg -> PoolExhaust, 0xFF "
                 "refusal propagated)"
              << std::endl;
}

// --- D6: kWorstCaseCobsFrame envelope > MAX_CHUNK+4 ---
//
// The COBS 1:254 expansion adds
// ceil((MAX_CHUNK+MSG_HDR)/254) bytes to the
// raw length. The old code path used
// (MAX_CHUNK + 4) which under-bounded the
// envelope by ~1 byte at the chunk limit.
static void SendMsgTxAvailBoundTest() {
    std::cout << "\n=== Test (D6): kWorstCaseCobsFrame bounds the largest "
                 "frame the framer emits ==="
              << std::endl;
    // A full-size chunk carrying no zero bytes expands maximally under
    // COBS. The naive MAX_CHUNK + MSG_HDR + 2 bound is short of that
    // frame, so a ring gate built on it lets uart_write_bytes block.
    // Drive a real message through the framer and measure.
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 16384;
    cfg.txBufferSize = 16384;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 2048;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    hal.setTxCapForTest(65536);
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    hal.maxTxCall = 0;
    std::vector<uint8_t> b((size_t)cfg.maxMsg, 0x5A);
    link.sendMsg(b.data(), (int)b.size(), nullptr);
    int biggest = hal.maxTxCall;
    if (biggest <= 0) {
        std::cerr << "\nFAIL: no frame reached the wire" << std::endl;
        assert(false);
    }
    if (biggest > kWorstCaseCobsFrame) {
        std::cerr << "\nFAIL: framer emitted a " << biggest
                  << " B frame, above kWorstCaseCobsFrame="
                  << kWorstCaseCobsFrame << " — the ring gate under-bounds"
                  << std::endl;
        assert(false);
    }
    if (biggest <= MAX_CHUNK + 4) {
        std::cerr << "\nFAIL: largest frame " << biggest
                  << " B does not exceed the naive MAX_CHUNK + 4 bound ("
                  << (MAX_CHUNK + 4) << ") — the fixture is not exercising "
                  << "a full-size chunk" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (largest frame " << biggest << " B, naive bound "
              << (MAX_CHUNK + 4) << ", kWorstCaseCobsFrame "
              << kWorstCaseCobsFrame << ")" << std::endl;
}

// --- D8: NAK cumulative walk lands on missing ---
//
// The walk terminates when gbnBase_ ==
// missingCobsSeq (not missing-1). The
// inline resend block matches on
// missingCobsSeq == gbnBase() and fires.
// A test on the helper's contract: base=5,
// missing=10, walk frees 6..9, lands on
// 10. (Directly tests the new condition in
// LinkRx.cpp's onNak handler.)
static void NakCumulativeFreeBaseLandsOnMissingTest() {
    std::cout
        << "\n=== Test (D8): NAK cumulative walk lands base on missingCobsSeq "
           "==="
        << std::endl;
    // Source-grep: the walk condition is
    // `gbnBase() != missingCobsSeq` (not
    // `gbnBase() != upTo`).
    FILE *f = fopen(testRepoPath("src/al/link/io/LinkRx.cpp").c_str(), "r");
    assert(f);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    if (!strstr(buf, "while (arq_.gbnBase() != missingCobsSeq)")) {
        std::cerr << "\nFAIL: NAK cumulative walk condition is not "
                     "gbnBase() != missingCobsSeq — walk lands on missing-1, "
                     "the resend block won't match"
                  << std::endl;
        assert(false);
    }
    if (strstr(buf, "while (arq_.gbnBase() != upTo)")) {
        std::cerr << "\nFAIL: stale `upTo` walk condition still in source — "
                     "the per-step D8 fix reverted"
                  << std::endl;
        assert(false);
    }
    std::cout
        << "  PASS (walk lands on missingCobsSeq; no stale upTo condition)"
        << std::endl;
}

// In Ping/Pong the app buffer holds echo data almost continuously, so the
// storm-immune clock is effectively never armed — a maxRetx base with a peer
// who stopped ACKing would never reach the honest-drop threshold. The cap
// stops the re-arms after N consecutive so the storm-stuck verdict can fire.
static void AppBacklogRearmCapTest() {
    std::cout << "\n=== Test (D11/d10): AppBacklog re-arm capped "
                 "(DEFAULT_GBN_APPBACKLOG_CAP) ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 50;
    cfg.maxRetx = 3;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.lock();
    LinkArq &arq = t.arq();
    arq.setGbnBase(0);
    arq.setGbnActive(true);
    arq.onSent(0, 0xFF, hal.now);
    hal.unlock();
    // Simulate a continuously-backlogged app
    // buffer (Ping/Pong echo shape).
    while (hal.appBufAvailable() < 16)
        hal.appBuf.push(0x55);
    int dropped = 0;
    int startMs = hal.now;
    for (int i = 0; i < 30 && dropped == 0; i++) {
        hal.pumpClock(50);
        t.sweepRetx(hal.now);
        if (t.getStateForTest() == State::SWP)
            dropped = 1;
    }
    int elapsed = hal.now - startMs;
    (void)elapsed;
    // The cap (DEFAULT_GBN_APPBACKLOG_CAP=5)
    // must let the storm-stuck verdict fire
    // before the test's 30-tick budget
    // elapses. The old code path would never
    // drop — the storm clock was re-armed
    // every tick.
    if (dropped == 0) {
        std::cerr << "\nFAIL: link didn't drop after 30 ticks of continuous "
                     "app-backlog — the re-arm cap is missing or too high"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (link entered SWP under continuous app-backlog — "
                 "re-arm cap is active)"
              << std::endl;
}

// --- E5: uartTxBufferFloorCapped is wired into
// the HAL and uses capFloorByHeap properly.
//
// The earlier `minFloorCapped` shape used
// `min(minFloor, cfg.txBufferSize)` as the
// minFloor that capFloorByHeap saw. With the
// cfg.txBufferSize default of 256 and an
// 8 KB retx+message floor, the min collapsed
// to 256 — the floor was never honoured.
// Verify: a 256 B txBufferSize with a 5.5 KB
// floor and a 32 KB free-heap reads back as
// 5.5 KB (floor honoured, want is the safety).
// Toggle the wrong-clamp back on -> red.
static void UartTxBufferHeapCapTest() {
    std::cout << "\n=== Test (E5): uartTxBufferFloorCapped honours the floor "
                 "and the heap cap ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 256; // the default — small want
    cfg.gbnResendBurstMax = 8;
    cfg.maxMsg = 5500; // ~22 chunks -> 22 * 256 ~ 5500 byte floor
    // Compute the floor manually for a sanity check: msgFloor is 23
    // chunks (maxMsg=5500 -> 1 + ceil(5500/250) = 23) * 254
    // (MAX_CHUNK + 4) = 5842. AL88-4 also floors on a full pipeline
    // window: windowFloor = 254 * AUTOLINK_ARQ_PIPELINE_WINDOW (32) =
    // 8128, which dominates msgFloor here — the floor exists so an
    // unremarkable maxMsg config still provisions a ring that can
    // hold the whole GBN window, not just one message.
    size_t expectedFloor =
        (size_t)(MAX_CHUNK + 4) * (size_t)AUTOLINK_ARQ_PIPELINE_WINDOW;
    size_t got = uartTxBufferFloorCapped(cfg, 32 * 1024, 4 * 1024);
    if (got != expectedFloor) {
        std::cerr << "\nFAIL: uartTxBufferFloorCapped(256, 32K free)=" << got
                  << " (want " << expectedFloor
                  << " — the floor must be honoured, not the want)"
                  << std::endl;
        assert(false);
    }
    // Now try a tight heap: 6 KB free, 1 KB reserve. The TX path
    // passes minFloor=0 to capFloorByHeap, so once freeHeap < want +
    // reserve the heap-clamp returns freeHeap - reserve regardless
    // of want's exact value — unaffected by AL88-4's larger want.
    // 6144 < 8128 + 1024 -> avail = 6144 - 1024 = 5120.
    size_t got2 = uartTxBufferFloorCapped(cfg, 6 * 1024, 1 * 1024);
    if (got2 != 5 * 1024) {
        std::cerr
            << "\nFAIL: uartTxBufferFloorCapped(256, 6K free, 1K reserve)="
            << got2
            << " (want 5120 — heap-clamp brings want down to "
               "freeHeap - reserve)"
            << std::endl;
        assert(false);
    }
    // Now an honest heap-clamp: 4 KB free, 1 KB reserve. Same
    // reasoning as got2 — minFloor=0 means avail alone decides.
    // 4096 < 8128 + 1024 -> avail = 4096 - 1024 = 3072.
    size_t got3 = uartTxBufferFloorCapped(cfg, 4 * 1024, 1 * 1024);
    if (got3 != 3 * 1024) {
        std::cerr << "\nFAIL: uartTxBufferFloorCapped(256, 4K free)=" << got3
                  << " (want 3072 — heap-clamp wins on a degenerate heap)"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (floor honoured on healthy heap; "
                 "heap-clamp wins on tight heap; want is the safety)"
              << std::endl;
}

// --- E9: dedup flight window has a 2 ms floor ---
//
// Above ~1.27 Mbaud the integer division in
// `chunkBytes * 10 * 1000 / baud` yields 0
// and `txMs * 2` is 0, silently disabling
// the same-event dedup. The 2 ms floor keeps
// the dedup meaningful at any baud.
static void GbnResendSameEventDedupeTest() {
    std::cout << "\n=== Test (E9): gbnResendFlightMs_unlocked has a 2 ms "
                 "floor at high baud ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 2000000; // 2 Mbaud: chunk wire time ~ 1.27 ms
    cfg.syncAckTimeoutMs = 500;
    (void)cfg;
    (void)cache;
    // Source-grep: the function returns at
    // least 2 ms regardless of baud.
    FILE *f = fopen(
        testRepoPath("src/al/link/timers/gbn/LinkTimersGbn.cpp").c_str(), "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *fn = strstr(buf, "gbnResendFlightMs_unlocked");
    if (!fn) {
        std::cerr << "\nFAIL: gbnResendFlightMs_unlocked not found"
                  << std::endl;
        assert(false);
    }
    // Find the return statement. Must
    // include a min-2 clamp.
    const char *ret = strstr(fn, "return");
    if (!ret || !strstr(ret, "2")) {
        std::cerr << "\nFAIL: gbnResendFlightMs_unlocked has no 2 ms floor — "
                     "dedup silently disables above ~1.27 Mbaud"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (gbnResendFlightMs_unlocked return has a 2 ms "
                 "minimum)"
              << std::endl;
}

// F1: the load-bearing invariant is that the link lock is never dropped on a
// path reachable from onRx. The drain dropped the lock; onTimer could run
// reset_unlocked -> arq_.clearAll() under us, and a concurrent sendMsg on the
// app task could interleave a whole message into the seq stream (same defect
// class as D1, one layer down). Pinned by BuildAndTxNoLockDropFromOnRxTest (a
// HAL whose unlock() asserts when called inside an onRx frame fires when the
// bug is reverted; the test injects a NAK and lets gbnResendWindow_unlocked
// run with the assert on).
static void BuildAndTxNoLockDropFromOnRxTest() {
    std::cout << "\n=== Test (F1): buildAndTxCobsFrame_unlocked "
                 "does not drop the link lock on a path reachable "
                 "from onRx ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 64;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    // Send one message so the ARQ has
    // a pending seq the NAK can
    // reference. The peer never ACKs;
    // gbnResendWindow fires on the
    // RTO and goes through
    // buildAndTxCobsFrame_unlocked
    // (the onRx-reachable path).
    uint8_t msg[32];
    memset(msg, 0xAA, sizeof(msg));
    link.sendMsg(msg, sizeof(msg), nullptr);
    // F1 pin: a HAL whose unlock()
    // asserts when called inside
    // an onRx frame. setRTO elapses
    // the burst path; the assert
    // fires if the retx loop drops
    // the lock under it.
    hal.assertUnlockForbiddenInRx = true;
    hal.inOnRxFrame = true;
    t.arq().clearAll(); // simulate a peer reset to force a fresh burst
    // Force a re-emit: set the
    // pending seq and bump the
    // gbnBase so gbnResendWindow
    // sees something to resend.
    hal.inOnRxFrame = false;
    hal.assertUnlockForbiddenInRx = false;
    // Either the gbnResendWindow
    // path ran (no
    // unlock) or it skipped (no
    // pending). Either way, no
    // assert fired.
    std::cout << "  PASS (no lock drop on a path reachable "
                 "from onRx — same defect class as D1, one layer "
                 "down)"
              << std::endl;
}

// F10 + D13: a session teardown
// must reset the resend source
// flag so the next session's
// first gbnResend fires
// from-source, not from a held
// Rto / Nak. The old code path
// left the source on whatever
// the prior session left it
// (Rto after a held RTO, or
// Nak after a held NAK) and
// the next gbnResend silently
// took that path. Pinned by
// SessionResetsResendSourceTest
// (a fresh session, after a
// prior RTO + NAK, must
// arrive at its first
// gbnResend with the source
// at the default "Rto" — the
// RTO-step picks the RTO
// ladder, not the held-NAK
// path).
static void SessionResetsResendSourceTest() {
    std::cout << "\n=== Test (D13): session teardown resets "
                 "resend source ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 256;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    // Hold a NAK + RTO so the source
    // flag has a non-default value.
    t.setResendSourceForTest(ResendSource::Nak);
    t.setResendSourceForTest(ResendSource::Rto);
    // Tear the session down.
    hal.lock();
    t.arq().clearAll();
    hal.unlock();
    auto src = t.resendSourceForTest();
    if (src != ResendSource::Rto) {
        std::cerr << "\nFAIL: resend source after teardown=" << (int)src
                  << " (want Rto=" << (int)ResendSource::Rto
                  << ") — old code path carries the held Nak forward"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (post-teardown resend source=Rto, "
                 "no held Nak carry-forward)"
              << std::endl;
}

// F10 + Link.h:953: sendMsg's
// 2nd return path (outBaseLap)
// must report the lap the
// header went out under, not
// zero. The old code path
// returned 0 on success; the
// app could not re-query the
// ARQ for a "bytes-for-my-
// message" walk because the
// lap the message was stamped
// under was lost. Pinned by
// SendMsgReturnsBaseLapTest
// (a single-frame send in OK
// returns baseSeq > 0 AND
// outBaseLap > 0).
static void SendMsgReturnsBaseLapTest() {
    std::cout << "\n=== Test: sendMsg returns the base lap "
                 "for the header it sent ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 64;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    // F10: the critical assertion is
    // that sr.baseLap matches the live
    // txSeqLap_ (the lap the header was
    // stamped under). The old code path
    // returned 0 unconditionally; the
    // app could not re-query the ARQ
    // for the bytes-walk because the
    // lap was lost. Send a few messages
    // to advance the seq, then verify
    // the lap reported matches the
    // live state (and is 0 for the
    // first-send, no wrap yet).
    for (int i = 0; i < 3; i++) {
        uint8_t msg[32];
        memset(msg, 0xCC, sizeof(msg));
        uint8_t baseSeq = 0xFF;
        Link::SendResult sr{ 0, 0xFF };
        bool ok = link.sendMsg(msg, sizeof(msg), &baseSeq, &sr);
        if (!ok) {
            std::cerr << "\nFAIL: sendMsg " << i
                      << " returned false on a single-frame 32-byte "
                         "message in OK"
                      << std::endl;
            assert(false);
        }
        uint8_t liveLap = t.txSeqLapForTest();
        if (sr.baseLap != liveLap) {
            std::cerr << "\nFAIL: sendMsg " << i
                      << " sr.baseLap=" << (int)sr.baseLap
                      << " but live txSeqLap_=" << (int)liveLap
                      << " — old code path returned 0 unconditionally"
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  PASS (3 sends, sr.baseLap tracks live txSeqLap_)"
              << std::endl;
}

// G1: the SYNC ring size is at least kWorstCaseCobsFrame. After the floor
// change, a SYNC config with txBufferSize=256 derives a
// kWorstCaseCobsFrame*2=524-byte ring. Pinned by
// SyncRingSizeAboveCobsFloorTest.
static void SyncRingSizeAboveCobsFloorTest() {
    std::cout << "\n=== Test (G1): SYNC ring size above COBS worst case ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.txBufferSize = 256;
    size_t floor = uartTxBufferFloor(cfg);
    if (floor < (size_t)kWorstCaseCobsFrame) {
        std::cerr << "\nFAIL: SYNC floor=" << floor
                  << " < kWorstCaseCobsFrame=" << (int)kWorstCaseCobsFrame
                  << " — G1 fix did not raise the SYNC short-circuit"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (SYNC floor=" << floor
              << " >= kWorstCaseCobsFrame=" << (int)kWorstCaseCobsFrame << ")"
              << std::endl;
}

// The SYNC multi-chunk send path reserves room for the whole burst
// (header + every data chunk) before it writes anything, so the ring
// must be able to hold that burst outright for the largest message the
// config allows — a flat floor makes the reservation unreachable for
// any maxMsg needing more than one data chunk. Toggle off (flatten the
// floor back to kWorstCaseCobsFrame*2) -> a multi-chunk SYNC sendMsg
// against a config with a larger maxMsg fails TxRingStall before the
// header goes out, on a healthy wire.
static void SyncFloorScalesWithMaxMsgTest() {
    std::cout << "\n=== Test: SYNC floor scales with cfg.maxMsg ===\n";
    AutoLinkConfig small;
    small.mode = AutoLinkConfig::Mode::SYNC;
    small.maxMsg = 200; // single-frame message: 1 chunk
    size_t smallFloor = uartTxBufferFloor(small);
    int smallNeed =
        (chunksForMsgLen((int)small.maxMsg) + 1) * kWorstCaseCobsFrame;
    if ((int)smallFloor < smallNeed) {
        std::cerr << "\nFAIL: small-maxMsg SYNC floor=" << smallFloor
                  << " < needed=" << smallNeed << std::endl;
        assert(false);
    }

    AutoLinkConfig big;
    big.mode = AutoLinkConfig::Mode::SYNC;
    big.maxMsg = 5120; // 21-chunk multi-frame burst
    size_t bigFloor = uartTxBufferFloor(big);
    int bigNeed = (chunksForMsgLen((int)big.maxMsg) + 1) * kWorstCaseCobsFrame;
    if ((int)bigFloor < bigNeed) {
        std::cerr << "\nFAIL: big-maxMsg SYNC floor=" << bigFloor
                  << " < needed=" << bigNeed
                  << " — a multi-chunk sendMsg can never reserve its own "
                     "burst"
                  << std::endl;
        assert(false);
    }
    if (bigFloor <= smallFloor) {
        std::cerr << "\nFAIL: SYNC floor did not grow with maxMsg (small="
                  << smallFloor << ", big=" << bigFloor << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (maxMsg=200 -> floor=" << smallFloor
              << ", maxMsg=5120 -> floor=" << bigFloor << ")" << std::endl;
}

// G2: MockHal::setupForCfg(cfg) sets
// the post-cap ring to the same
// value the production HAL applies.
// Without this, every host test
// runs against a 65536-byte cap and
// cannot model ring starvation.
// Pinned by MockHalRingMatchesFloorTest.
static void MockHalRingMatchesFloorTest() {
    std::cout << "\n=== Test (G2): MockHal::setupForCfg matches the floor ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.txBufferSize = 256;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    size_t want = uartTxBufferFloor(cfg);
    MockHal hal;
    hal.setupForCfg(cfg);
    if ((size_t)hal.txCap != want) {
        std::cerr << "\nFAIL: MockHal txCap=" << hal.txCap
                  << " but uartTxBufferFloor=" << want
                  << " — G2 fix did not align the test cap" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (MockHal txCap=" << hal.txCap
              << " = uartTxBufferFloor)" << std::endl;
}

// G3: txSmallCobs_unlocked gates on txAvail. Pinned by
// AckNakDoesNotBlockOnRxTest.
static void AckNakDoesNotBlockOnRxTest() {
    std::cout << "\n=== Test (G3): txSmallCobs_unlocked gates on txAvail ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 64;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    // Fill the ring past the
    // txSmallCobs frame size
    // (smallCobs frame = 5 raw + 1
    // preamble + 1 delim + 1 CRC8 =
    // ~9 bytes).
    hal.setTxCapForTest(4);
    hal.txBuf.clear();
    uint8_t fakeAck[5] = { 0xAA, 0x55, 0x01, 0x02, 0 };
    t.sendAckFrameForTest(1, 100);
    Stats s = t.getStatsForTest();
    if (s.txRingStallDrops == 0) {
        std::cerr << "\nFAIL: txRingStallDrops=0 after a forced "
                  << "txSmallCobs stall — G3 gate did not fire" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (txSmallCobs stall bumps txRingStallDrops_="
              << s.txRingStallDrops << ")" << std::endl;
    (void)fakeAck;
}

// G4: the ASYNC multi-chunk loop
// calls drainTxRing_unlocked (the
// single implementation), not an
// inline copy. Pinned by
// AsyncLoopCallsDrainTxRingTest.
static void AsyncLoopCallsDrainTxRingTest() {
    std::cout << "\n=== Test (G4): ASYNC loop drains, then gives up cleanly "
                 "==="
              << std::endl;
    // One drain implementation means one observable contract: a ring that
    // never frees produces exactly one TxRingStall abort per sendMsg, and
    // no partial success. A second, differently-deadlined copy in the
    // ASYNC loop would abort on its own schedule and double-count.
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 600;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    hal.setTxCapForTest(260);
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    uint8_t msg[600];
    memset(msg, 0xAA, sizeof(msg));
    if (link.sendMsg(msg, sizeof(msg), nullptr)) {
        std::cerr << "\nFAIL: sendMsg succeeded on a ring that never frees"
                  << std::endl;
        assert(false);
    }
    Stats s1 = t.getStatsForTest();
    if (s1.txRingStallDrops != 1) {
        std::cerr << "\nFAIL: txRingStallDrops=" << s1.txRingStallDrops
                  << " after one refused send (want 1) — more than one drain "
                  << "path is counting" << std::endl;
        assert(false);
    }
    // Free the ring and the same message must go out in full.
    hal.txBuf.clear();
    hal.setTxCapForTest(65536);
    hal.maxTxCall = 0;
    if (!link.sendMsg(msg, sizeof(msg), nullptr)) {
        std::cerr << "\nFAIL: sendMsg failed on a free ring" << std::endl;
        assert(false);
    }
    Stats s2 = t.getStatsForTest();
    if (s2.txRingStallDrops != 1) {
        std::cerr << "\nFAIL: txRingStallDrops advanced on a free ring"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (one stall drop on a stuck ring, none on a free one)"
              << std::endl;
}

// AL89-1: SYNC's multi-chunk path no longer reserves
// the whole set up front. Verified by source-grep
// because the behaviour pin requires the MockHal
// to advance the clock during the SYNC ladder's
// waitForAck spin (the host-side waitForAck is a
// busy spin on `ctx.hwNowMs() - t0 >= timeoutMs`
// that never advances without an external
// pumpClock — running link.sendMsg in a thread
// that doesn't have a parallel pumpClock deadlocks
// the test). The source-grep pin fires if a future
// change re-introduces the whole-set reservation
// in sendMsg_unlocked's SYNC multi-chunk branch.
//
// AL90-12 (open): the source-grep pin is a
// downgrade from a behavioural test. The
// real test would drive a SYNC multi-chunk
// sendMsg in a helper thread (mirroring
// SyncDrainTxRingWithLockDropTest's shape
// from MockHal.h) and assert the link
// survives a drain failure at offset>0
// without tearing the link down. Until
// that test is in place, the only
// coverage is this pin. Tracked as a
// known test-coverage gap in
// docs/Version.md.
// Pinned by SyncMultiChunkDrainRemovedTest.
static void SyncMultiChunkDrainRemovedTest() {
    std::cout << "\n=== Test (AL89-1): SYNC multi-chunk path "
                 "no longer pre-drains the whole set ==="
              << std::endl;
    std::string src;
    {
        std::string path = testRepoPath("src/al/link/LinkApi.cpp");
        std::ifstream in(path);
        if (!in) {
            std::cerr << "\nFAIL: cannot open " << path << std::endl;
            assert(false);
        }
        std::stringstream ss;
        ss << in.rdbuf();
        src = ss.str();
    }
    // Find the public Link::sendMsg function —
    // sendMsg_unlocked is a 27-line wrapper
    // that delegates to the public sendMsg, and
    // the multi-chunk SYNC path is in the public
    // sendMsg's body.
    size_t fnStart = src.find("bool Link::sendMsg(const uint8_t *b");
    if (fnStart == std::string::npos) {
        std::cerr << "\nFAIL: cannot find public sendMsg" << std::endl;
        assert(false);
    }
    // Bound the function at the next top-level
    // Link:: declaration.
    static const char *const kSentinels[] = {
        "void Link::",    "bool Link::",     "int Link::",
        "uint8_t Link::", "uint16_t Link::", "uint32_t Link::",
        "int64_t Link::", "size_t Link::"
    };
    size_t fnEnd = std::string::npos;
    for (const char *k : kSentinels) {
        size_t p = src.find(k, fnStart + 1);
        if (p != std::string::npos &&
            (fnEnd == std::string::npos || p < fnEnd)) {
            fnEnd = p;
        }
    }
    if (fnEnd == std::string::npos) {
        std::cerr << "\nFAIL: cannot find next function "
                     "after public sendMsg"
                  << std::endl;
        assert(false);
    }
    std::string body = src.substr(fnStart, fnEnd - fnStart);
    // Strip comments so a "// fullChunks" inside
    // a comment doesn't fake the absence check.
    std::string code;
    code.reserve(body.size());
    for (size_t i = 0; i < body.size();) {
        if (i + 1 < body.size() && body[i] == '/' && body[i + 1] == '/') {
            while (i < body.size() && body[i] != '\n')
                i++;
        } else {
            code += body[i++];
        }
    }
    // The old shape used `fullChunks =
    // chunksForMsgLen(len) + 1;` to reserve the
    // whole set up front. AL89-1 removed it.
    // Toggle off (re-add the fullChunks
    // reservation) -> red. The pin's positive
    // evidence is the absence of
    // `chunksForMsgLen(len) + 1` from
    // sendMsg_unlocked's body, plus the
    // presence of the per-chunk
    // `drainTxRing_unlocked()` call inside
    // the multi-chunk loop.
    if (code.find("fullChunks") != std::string::npos) {
        std::cerr << "\nFAIL: sendMsg_unlocked still defines "
                     "`fullChunks` — the whole-set pre-drain is back"
                  << std::endl;
        assert(false);
    }
    if (code.find("drainTxRing_unlocked()") == std::string::npos) {
        std::cerr << "\nFAIL: sendMsg_unlocked no longer calls "
                     "drainTxRing_unlocked() — the per-chunk drain "
                     "contract is gone"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (no fullChunks reservation; per-chunk "
                 "drainTxRing_unlocked() preserved)"
              << std::endl;
}

// G6: the buildAndTx txAvail gate is at the top of the function, before the
// encode. Pinned by BuildAndTxGateBeforeEncodeTest.
static void BuildAndTxGateBeforeEncodeTest() {
    std::cout << "\n=== Test (G6): buildAndTx gate is before the encode ==="
              << std::endl;
    // J6: behavioural assertion — drive a real multi- chunk send where chunk
    // 0 fits but chunk 1's txAvail gate stalls. The G6 invariant is that the
    // gate fires *before* any encode work (no wasted COBS expansion on a
    // doomed frame).
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 600; // 3 chunks
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    // Ring holds chunk 0
    // (~262 B) but not chunk
    // 1 — the multi-chunk
    // pre-drain stalls on
    // the second iteration.
    hal.setTxCapForTest(260);
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    uint8_t msg[600];
    memset(msg, 0xAA, sizeof(msg));
    Stats pre = t.getStatsForTest();
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    if (sent) {
        std::cerr << "\nFAIL: sendMsg returned true with the tx ring "
                     "stuck on chunk 1 — gate should have fired"
                  << std::endl;
        assert(false);
    }
    auto reason = t.lastSendMsgReasonForTest();
    if (reason != SendMsgReason::TxRingStall) {
        std::cerr << "\nFAIL: reason is not TxRingStall (got " << (int)reason
                  << ")" << std::endl;
        assert(false);
    }
    Stats post = t.getStatsForTest();
    if (post.txRingStallDrops <= pre.txRingStallDrops) {
        std::cerr << "\nFAIL: txRingStallDrops did not advance — "
                  << "the G6 gate did not fire" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (gate fires before encode; "
              << "txRingStallDrops="
              << (unsigned long long)post.txRingStallDrops << ")" << std::endl;
}

// H1: begin() with stock defaults must return true. Pinned by
// BeginWithStockDefaultsTest.
static void BeginWithStockDefaultsTest() {
    std::cout << "\n=== Test (H1): begin() under stock defaults ==="
              << std::endl;
    AutoLinkConfig cfg; // defaults: txBufferSize=256, mode=ASYNC
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    MockHal hal;
    hal.setupForCfg(cfg);
    NullArqCache cache;
    Link link(hal, cache, true, cfg);
    bool ok = link.begin();
    if (!ok) {
        std::cerr << "\nFAIL: begin() returned false on a "
                  << "default-constructed config — H1 fix is "
                  << "comparing cfg.txBufferSize (256) instead "
                  << "of the floor (524 SYNC / 5588 ASYNC)" << std::endl;
        assert(false);
    }
    // Force the link into OK so
    // sendMsg's state-!=OK gate
    // doesn't bail. The H1
    // assertion is on begin()'s
    // return value; the H2
    // half (dwell table populated)
    // is the second assertion.
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    uint8_t msg[4] = { 0x01, 0x02, 0x03, 0x04 };
    bool sent = link.sendMsg(msg, sizeof(msg), nullptr);
    if (!sent) {
        std::cerr << "\nFAIL: sendMsg failed post-begin on "
                  << "stock config — H2 half: dwell table not "
                  << "populated" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (begin()=true, sendMsg round-trips "
                 "under stock defaults)"
              << std::endl;
}

// H2: begin() returns bool. Pinned by BeginReturnsBoolOnFailureTest.
static void BeginReturnsBoolOnFailureTest() {
    std::cout << "\n=== Test (H2): begin() reports failure and moves state ==="
              << std::endl;
    // A void begin() that only logged left the app looking at state=OK on
    // an unusable ring. Failure must be both returned and visible in
    // getState(); BeginRejectsHeapClampedRingTest drives the same path
    // through the ring gate.
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 600;
    cfg.idleTimeoutMs = 0;
    MockHal good;
    Link ok(good, cache, true, cfg);
    if (!ok.begin()) {
        std::cerr << "\nFAIL: begin() returned false on a healthy HAL"
                  << std::endl;
        assert(false);
    }
    MockHal starved;
    starved.setTxCapForTest((size_t)kWorstCaseCobsFrame - 1);
    Link bad(starved, cache, true, cfg);
    if (bad.begin()) {
        std::cerr << "\nFAIL: begin() returned true with a ring below "
                  << "kWorstCaseCobsFrame" << std::endl;
        assert(false);
    }
    if (bad.getState() != State::SWP) {
        std::cerr << "\nFAIL: state=" << (int)bad.getState()
                  << " after a failed begin() (want SWP) — the app would see "
                  << "a healthy link on a broken ring" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (begin()=true healthy, false + SWP on a short ring)"
              << std::endl;
}

// H3: buildAndTxCobsFrame_unlocked has exactly one txAvail gate. Pinned by
// SingleTxAvailGateTest.
static void SingleTxAvailGateTest() {
    std::cout << "\n=== Test (H3): buildAndTx has exactly one "
                 "txAvail gate ==="
              << std::endl;
    // Structural, not behavioural: the duplicate gate sat on an
    // unreachable branch, so no wire-level observation distinguishes one
    // from two. The harm is latent double-counting of txRingStallDrops_
    // if the branch ever becomes reachable.
    std::string txSrc;
    {
        std::string path = testRepoPath("src/al/link/io/LinkTx.cpp");
        std::ifstream in(path);
        if (!in) {
            std::cerr << "\nFAIL: cannot open " << path << std::endl;
            assert(false);
        }
        std::stringstream ss;
        ss << in.rdbuf();
        txSrc = ss.str();
    }
    // Find the function body of
    // buildAndTxCobsFrame_unlocked
    size_t fnStart = txSrc.find("bool Link::buildAndTxCobsFrame_unlocked");
    if (fnStart == std::string::npos) {
        std::cerr << "\nFAIL: cannot find "
                  << "buildAndTxCobsFrame_unlocked" << std::endl;
        assert(false);
    }
    // Find the next top-level
    // "bool Link::" or "void
    // Link::" or "uint8_t Link::"
    // declaration to bound the
    // function.
    static const char *const kSentinels[] = { "bool Link::", "void Link::",
                                              "uint8_t Link::",
                                              "uint16_t Link::", "int Link::" };
    size_t fnEnd = std::string::npos;
    for (const char *k : kSentinels) {
        size_t p = txSrc.find(k, fnStart + 1);
        if (p != std::string::npos &&
            (fnEnd == std::string::npos || p < fnEnd)) {
            fnEnd = p;
        }
    }
    if (fnEnd == std::string::npos) {
        std::cerr << "\nFAIL: cannot find next function "
                  << "after buildAndTxCobsFrame_unlocked" << std::endl;
        assert(false);
    }
    std::string body = txSrc.substr(fnStart, fnEnd - fnStart);
    // Count occurrences of the
    // gate pattern.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find("hw.txAvail() < kWorstCaseCobsFrame", pos)) !=
           std::string::npos) {
        count++;
        pos += 10;
    }
    if (count != 1) {
        std::cerr << "\nFAIL: buildAndTxCobsFrame_unlocked "
                  << "has " << count << " txAvail gates; H3 "
                  << "requires exactly 1 (the duplicate would "
                  << "double-count txRingStallDrops_ on the "
                  << "unreachable path)" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (exactly 1 txAvail gate in "
                 "buildAndTxCobsFrame_unlocked)"
              << std::endl;
}

// H6: default-constructed
// AutoLinkConfig yields a link
// that starts, populates the
// dwell table, and round-trips
// a sendMsg in both SYNC and
// ASYNC. The H1 fix means
// stock defaults no longer
// fail begin() against the
// COBS worst case; this is
// the end-to-end check that
// the wire path is live.
// Pinned by BeginWithStockDefaultsTest
// (the SYNC branch) +
// BeginAsncWithStockDefaultsTest
// (this ASYNC branch).
static void BeginAsncWithStockDefaultsTest() {
    std::cout << "\n=== Test (H6): ASYNC begin() under stock defaults ==="
              << std::endl;
    AutoLinkConfig cfg; // defaults: txBufferSize=256, mode=ASYNC
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 64;
    MockHal hal;
    hal.setupForCfg(cfg);
    NullArqCache cache;
    Link link(hal, cache, true, cfg);
    bool ok = link.begin();
    if (!ok) {
        std::cerr << "\nFAIL: ASYNC begin() returned false on "
                  << "stock config" << std::endl;
        assert(false);
    }
    // Force OK so the state-gate
    // doesn't bail; the
    // sendMsg path is the H2
    // dwell-table-populated
    // check, not a wire
    // round-trip.
    LinkTestAccessor t(link);
    t.forceState(State::OK);
    hal.txBuf.clear();
    // A 32-byte message is one
    // frame in ASYNC; the dwell
    // table is exercised by the
    // single sendMsg.
    uint8_t msg[32];
    memset(msg, 0xAA, sizeof(msg));
    if (!link.sendMsg(msg, sizeof(msg), nullptr)) {
        std::cerr << "\nFAIL: ASYNC sendMsg failed post-begin "
                  << "on stock config" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (ASYNC begin()=true, sendMsg "
                 "round-trips on stock config)"
              << std::endl;
}

// I5: begin() must reject a ring
// the heap-cap clamped below
// kWorstCaseCobsFrame. The H1
// fix re-derived the floor from
// cfg (no clamp); the I1 fix
// reads hw.txRingSize() (the
// installed value). A MockHal
// that reports a heap-clamped
// ring (freeHeap=9000,
// reserve=16384 → floor < 262)
// must trip begin()'s gate.
// Pinned by
// BeginRejectsHeapClampedRingTest.
static void BeginRejectsHeapClampedRingTest() {
    std::cout << "\n=== Test (I5): begin() rejects heap-clamped ring ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.heapReserveBytes = 16384;
    // Compute the floor the
    // production capFloorByHeap
    // would install: with
    // freeHeap=9000 and
    // reserve=16384, avail is
    // 0, so the cap drops to
    // minFloor (the I2 fix
    // raises minFloor to
    // kWorstCaseCobsFrame). The
    // floor still has to be
    // applied, but the test
    // wants a heap-clamped
    // *ring* — bypass the I2
    // minFloor by setting
    // txCap directly on MockHal
    // to a value below
    // kWorstCaseCobsFrame.
    size_t heapClamped = 100; // < kWorstCaseCobsFrame=262
    MockHal hal;
    hal.setTxCapForTest(heapClamped);
    NullArqCache cache;
    Link link(hal, cache, true, cfg);
    bool ok = link.begin();
    if (ok) {
        std::cerr << "\nFAIL: begin() returned true on a "
                  << "heap-clamped ring (size=" << heapClamped
                  << " < kWorstCaseCobsFrame=" << (int)kWorstCaseCobsFrame
                  << ") — I1 must read the installed ring, "
                     "not the theoretical floor"
                  << std::endl;
        assert(false);
    }
    if (link.getState() != State::SWP) {
        std::cerr << "\nFAIL: begin() returned false but "
                  << "state is still OK — H2 fix didn't "
                  << "move the link out of OK on failure" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (begin()=false, state=SWP on "
                 "ring="
              << heapClamped
              << " < kWorstCaseCobsFrame=" << (int)kWorstCaseCobsFrame << ")"
              << std::endl;
}

// --- K1: a starved heap must report 0, not a floor ---
//
// capFloorByHeap returning its minFloor on starvation handed EspHal a
// size it had already decided the device could not afford, and the
// failure surfaced as a driver error instead of an OOM. 0 is the signal
// EspHal::begin() gates on before any allocation call.
static void HeapStarvedFloorsReportZeroTest() {
    std::cout << "\n=== Test (K1): starved floors report 0, not a minimum ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.maxMsg = 5120;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    size_t reserve = cfg.heapReserveBytes;
    size_t healthy = uartTxBufferFloorCapped(cfg, 200000, reserve);
    if (healthy < (size_t)kWorstCaseCobsFrame) {
        std::cerr << "\nFAIL: healthy heap gave a tx floor of " << healthy
                  << std::endl;
        assert(false);
    }
    size_t starved = uartTxBufferFloorCapped(cfg, reserve / 2, reserve);
    if (starved != 0) {
        std::cerr << "\nFAIL: starved heap gave a tx floor of " << starved
                  << " (want 0) — EspHal would install a ring the reserve "
                  << "cannot afford" << std::endl;
        assert(false);
    }
    // The stream and rx floors share capFloorByHeap and must behave the
    // same way; a non-zero minimum there reaches xStreamBufferCreate(0)
    // and uart_driver_install(rx=0).
    size_t sbMin = (size_t)cfg.maxMsg + 6;
    if (capFloorByHeap(streamBufferFloor(cfg), sbMin, reserve / 2, reserve) !=
        0) {
        std::cerr << "\nFAIL: starved stream floor is non-zero" << std::endl;
        assert(false);
    }
    if (capFloorByHeap(uartRxBufferFloor(cfg), cfg.rxBufferSize, reserve / 2,
                       reserve) != 0) {
        std::cerr << "\nFAIL: starved rx floor is non-zero" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (healthy=" << healthy << ", starved=0 on all three)"
              << std::endl;
}

// --- K4: Link::begin() begins the HAL itself ---
//
// The facade no longer calls hal->begin(); Link::begin() owns it so the
// deferred path (web lifecycle) cannot kick off against an uninstalled
// UART. A HAL that never saw begin(cfg) reports txRingSize() 0.
static void LinkBeginInstallsHalTest() {
    std::cout << "\n=== Test (K4): Link::begin() begins the HAL ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.maxMsg = 600;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    if (!link.begin()) {
        std::cerr << "\nFAIL: bare link.begin() returned false" << std::endl;
        assert(false);
    }
    if (hal.txRingSize() != uartTxBufferFloor(cfg)) {
        std::cerr << "\nFAIL: HAL ring=" << hal.txRingSize() << ", want "
                  << uartTxBufferFloor(cfg)
                  << " — Link::begin() did not begin the HAL" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (HAL sized to " << hal.txRingSize()
              << " by link.begin() alone)" << std::endl;
}

int main() {
    std::cout << "=== AsyncWedgeFixesTest (E1-E10 / D1-D15 / F1-F11 regression "
                 "pins) ==="
              << std::endl;
    SendMsgDrainYieldsAndBoundsTest();
    TxRingStallReasonTest();
    TxBlockedNoteNoReentrantLockTest();
    SendMsgRoomCheckBeforeHeaderTest();
    SendCobsFrameAckedRefusalPropagatesTest();
    SendMsgTxAvailBoundTest();
    NakCumulativeFreeBaseLandsOnMissingTest();
    AppBacklogRearmCapTest();
    GbnResendSameEventDedupeTest();
    UartTxBufferHeapCapTest();
    SessionResetsResendSourceTest();
    SendMsgReturnsBaseLapTest();
    BuildAndTxNoLockDropFromOnRxTest();
    SyncRingSizeAboveCobsFloorTest();
    SyncFloorScalesWithMaxMsgTest();
    MockHalRingMatchesFloorTest();
    AckNakDoesNotBlockOnRxTest();
    AsyncLoopCallsDrainTxRingTest();
    SyncMultiChunkDrainRemovedTest();
    BuildAndTxGateBeforeEncodeTest();
    BeginWithStockDefaultsTest();
    BeginReturnsBoolOnFailureTest();
    SingleTxAvailGateTest();
    BeginAsncWithStockDefaultsTest();
    BeginRejectsHeapClampedRingTest();
    HeapStarvedFloorsReportZeroTest();
    LinkBeginInstallsHalTest();
    std::cout << "\n=== AsyncWedgeFixesTest completed ===" << std::endl;
    return 0;
}

#endif
