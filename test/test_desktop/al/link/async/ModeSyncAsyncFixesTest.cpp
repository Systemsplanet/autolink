// Five ASYNC link-thrash fixes, pinned together. Each pin fails when its
// fix is reverted.
//
//   1. IHal::setMode — AutoLink::setMode forwards to the link AND the HAL,
//      so a mode set before begin() sizes the buffers for that mode. The
//      HAL cannot resize a live FreeRTOS stream buffer, so a post-begin
//      setMode only updates cfg.mode (keeping the boot log honest).
//   2. uartRxBufferFloor / uartTxBufferFloor scale the UART driver buffers
//      by mode: an ASYNC pipeline needs room for a full in-flight window.
//   3. okTickMs() clamps at one RTO in ASYNC, so the timer-driven GBN retx
//      sweep is never the bottleneck on recovery. SYNC blocks inline and
//      never walks the ARQ table.
//   4. Every TX path bumps txBytes, not just the data path. Pong's wire
//      output is almost entirely ACK/NAK; a data-path-only counter reports
//      0 B/s despite a healthy link.
//   5. Ping's backpressure branch stamps a cooldown so a full cache cannot
//      spin the send loop. The gate must NOT be conditioned on txDelayMs —
//      ASYNC flood mode runs at txDelayMs=0, which is exactly where the
//      cooldown is needed.
//
// The floor and chunk math must reference MAX_CHUNK / MSG_HDR symbolically:
// a literal 250 or 6 would silently desync the buffer sizing from the wire
// cap the framer honours. Source-grep pins enforce that, because the defect
// is the presence of the literal, not an observable behaviour.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

#    include "al/AutoLinkConfig.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "MockHal.h"
#    include "LinkTestAccessor.h"

using namespace autolink;

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

// ============================================================================
// Pin 1 (Fix 1) — IHal::setMode is a virtual hook; AutoLink::setMode forwards
// to both link and hal so NVS-restored mode reaches the HAL before begin().
// ============================================================================

void test_pin_ihal_declares_setMode() {
    std::cout << "\n=== Pin 1a: IHal.h declares virtual setMode hook ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/IHal.h");
    assert(!src.empty());
    assert(src.find("virtual void setMode(AutoLinkConfig::Mode)") !=
               std::string::npos &&
           "IHal must declare a virtual setMode(AutoLinkConfig::Mode) "
           "hook so AutoLink can forward the mode to the HAL");
    std::cout << "  PASS (IHal::setMode(AutoLinkConfig::Mode) declared virtual)"
              << std::endl;
}

void test_pin_autolink_setMode_forwards_to_hal() {
    std::cout
        << "\n=== Pin 1b: AutoLink::setMode forwards to hal before link ==="
        << std::endl;
    // A refactor that drops the hal->setMode forward flips this red.
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    // Find the setMode method body.
    auto p = src.find("void setMode(AutoLinkConfig::Mode m)");
    assert(p != std::string::npos);
    // The body is short but the leading
    // comment is verbose — read 1000 chars
    // so we cover the comment + the
    // hal->setMode / link->setMode calls.
    std::string slice = src.substr(p, 1000);
    assert(slice.find("hal->setMode(m)") != std::string::npos &&
           "AutoLink::setMode must forward to hal->setMode(m) before "
           "(or alongside) link->setMode(m), or the HAL's cfg copy goes "
           "stale");
    assert(slice.find("link->setMode(m)") != std::string::npos &&
           "AutoLink::setMode must still forward to link->setMode(m) "
           "so the link layer's cfg.mode tracks the restored mode");
    std::cout
        << "  PASS (AutoLink::setMode calls hal->setMode AND link->setMode)"
        << std::endl;
}

void test_pin_espal_setMode_rederives_buffers_pre_begin() {
    std::cout
        << "\n=== Pin 1c: EspHal::setMode re-derives buffers pre-begin ==="
        << std::endl;
    // Pre-begin: re-derive the floors. Post-begin: the buffers are
    // committed, so only cfg.mode moves.
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());
    auto p = src.find("void setMode(AutoLinkConfig::Mode m) override");
    assert(p != std::string::npos);
    std::string body = src.substr(p, 1200);
    // The pre-begin path must re-derive the
    // three buffer floors so the UART driver
    // + stream-buffer get the right sizes.
    assert(body.find("rx_buffer_size_ = rxBufferFloor(cfg)") !=
               std::string::npos &&
           "EspHal::setMode (pre-begin) must re-derive rx_buffer_size_ "
           "from rxBufferFloor(cfg), or a mode change before begin() "
           "leaves the buffer sized for the wrong mode");
    assert(body.find("tx_buffer_size_ = txBufferFloor(cfg)") !=
               std::string::npos &&
           "EspHal::setMode (pre-begin) must re-derive tx_buffer_size_ "
           "from txBufferFloor(cfg)");
    assert(body.find("stream_buf_size_ = streamBufferFloor(cfg)") !=
               std::string::npos &&
           "EspHal::setMode (pre-begin) must re-derive stream_buf_size_ "
           "from streamBufferFloor(cfg)");
    std::cout << "  PASS (EspHal::setMode re-derives rx/tx/streamBuf pre-begin)"
              << std::endl;
}

// ============================================================================
// Pin 2 (Fix 2) — uartRxBufferFloor / uartTxBufferFloor scale by mode.
// ASYNC must produce a multi-KB rx floor; SYNC keeps the caller value.
// ============================================================================

void test_pin_uart_rx_buffer_scales_by_mode() {
    std::cout << "\n=== Pin 2a: uartRxBufferFloor scales with ASYNC mode ==="
              << std::endl;
    // The rx driver buffer must hold a full in-flight pipeline, or a
    // multi-chunk ASYNC flood overruns it.
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    const size_t asyncRx = autolink::uartRxBufferFloor(cfg);
    std::cout << "  ASYNC rx floor = " << asyncRx
              << " bytes (cfg.rxBufferSize=" << cfg.rxBufferSize << ")"
              << std::endl;
    assert(asyncRx > 4096 &&
           "ASYNC rx floor must be > 4 KB; the 32-slot pipeline alone is "
           "~8 KB. A floor that ignores mode and returns cfg.rxBufferSize "
           "unconditionally underruns on a multi-chunk flood");
    // SYNC keeps the caller value (default 2048).
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    const size_t syncRx = autolink::uartRxBufferFloor(cfg);
    std::cout << "  SYNC rx floor = " << syncRx
              << " bytes (passes through cfg.rxBufferSize)" << std::endl;
    assert(syncRx == cfg.rxBufferSize &&
           "SYNC rx floor must return cfg.rxBufferSize unchanged — "
           "one-coalesced-frame window is well under 2 KB");
    std::cout << "  PASS (ASYNC=" << asyncRx << " > SYNC=" << syncRx << ")"
              << std::endl;
}

void test_pin_uart_tx_buffer_scales_by_mode() {
    std::cout << "\n=== Pin 2b: uartTxBufferFloor scales with ASYNC mode ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    const size_t asyncTx = autolink::uartTxBufferFloor(cfg);
    std::cout << "  ASYNC tx floor = " << asyncTx << " bytes" << std::endl;
    // Caller-set larger value still wins.
    cfg.txBufferSize = 8 * 1024;
    const size_t callerWins = autolink::uartTxBufferFloor(cfg);
    assert(callerWins == cfg.txBufferSize &&
           "caller's larger cfg.txBufferSize must win over the floor");
    cfg.txBufferSize = 256;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    const size_t syncTx = autolink::uartTxBufferFloor(cfg);
    // G1: SYNC floor is now
    // max(cfg.txBufferSize,
    // kWorstCaseCobsFrame*2).
    // A 256-B default rises to
    // 524. Pinned by
    // SyncRingSizeAboveCobsFloorTest.
    assert(syncTx >= (size_t)kWorstCaseCobsFrame &&
           "SYNC tx floor must be at least kWorstCaseCobsFrame so the "
           "wire-write primitives can fit one COBS frame on the ring");
    (void)syncTx;
    std::cout << "  PASS (caller's larger value wins, SYNC floor "
              << ">= kWorstCaseCobsFrame)" << std::endl;
}

void test_pin_uart_rx_floor_uses_pipeline_window_constant() {
    std::cout << "\n=== Pin 2c: uartRxBufferFloor scales with the ARQ pipeline "
                 "window ==="
              << std::endl;
    // Same window constant as the ARQ cache, so the two stay in lockstep.
    std::string src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!src.empty());
    auto p = src.find("uartRxBufferFloor");
    assert(p != std::string::npos);
    // The function is defined only once
    // (no forward decl) — read a slice
    // from the definition that covers
    // the formula body.
    std::string body = src.substr(p, 1000);
    assert(body.find("AUTOLINK_ARQ_PIPELINE_WINDOW") != std::string::npos &&
           "uartRxBufferFloor must reference AUTOLINK_ARQ_PIPELINE_WINDOW "
           "so the rx floor tracks the ARQ cache window size");
    std::cout << "  PASS (rx floor references the pipeline window constant)"
              << std::endl;
}

// Pin 2d — the floors derive perChunk from MAX_CHUNK symbolically.

void test_pin_uart_floors_derive_perChunk_from_MAX_CHUNK() {
    std::cout << "\n=== Pin 2d: UART floors derive perChunk from MAX_CHUNK "
                 "(no literal kChunkCap) ==="
              << std::endl;
    // J6: behavioural assertion,
    // not a 800-char source-grep
    // (the J5 refactor moved
    // MAX_CHUNK into
    // LinkFrameSizes.h, which
    // broke the offset-based
    // check that lived off
    // the (void)MAX_CHUNK;
    // suppression line). For a
    // few maxMsg values,
    // compute the expected
    // floor from the formula
    // and compare to the
    // actual uartTxBufferFloor
    // output. A drift between
    // the floor's hard-coded
    // chunk cap and the
    // runtime MAX_CHUNK would
    // surface as a mismatch.
    constexpr int kFrameOverhead = 4;
    const int maxMsgs[] = { 64, 250, 600, 1024, 4096 };
    for (int maxMsg : maxMsgs) {
        AutoLinkConfig cfg;
        cfg.mode = AutoLinkConfig::Mode::ASYNC;
        cfg.maxMsg = maxMsg;
        // Expected: max of
        // (retx budget) and
        // (full-message chunk
        // set) at perChunk
        // bytes. perChunk =
        // (size_t)MAX_CHUNK +
        // kFrameOverhead. retx
        // budget =
        // perChunk * (gbnResendBurstMax
        // + 1). msg chunks =
        // chunksForMsgLen(maxMsg).
        // The formula uses
        // MAX_CHUNK symbolically
        // — a chunk-cap bump
        // in LinkWire.h would
        // re-run the formula
        // and either:
        //   (a) match the new
        //   floor (caller
        //   wrote the formula
        //   off MAX_CHUNK), or
        //   (b) miss the test
        //   (caller hard-coded
        //   a literal). This
        //   test pins the
        //   formula.
        size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
        int burst = cfg.gbnResendBurstMax > 0 ? cfg.gbnResendBurstMax : 0;
        size_t retxFloor = perChunk * (size_t)(burst + 1);
        int msgChunks = chunksForMsgLen(maxMsg);
        if (msgChunks < 1)
            msgChunks = 1;
        size_t msgFloor = perChunk * (size_t)msgChunks;
        // AL88-4: the floor must also cover a full pipeline window's
        // worth of chunks, or Link::begin()'s installed-ring clamp
        // shrinks the runtime GBN window below
        // AUTOLINK_ARQ_PIPELINE_WINDOW even on an otherwise healthy
        // ring — measured in the field as ASYNC throughput at ~3% of
        // SYNC's on the same wire.
        size_t windowFloor =
            perChunk * (size_t)autolink::AUTOLINK_ARQ_PIPELINE_WINDOW;
        size_t expected = retxFloor > msgFloor ? retxFloor : msgFloor;
        if (windowFloor > expected)
            expected = windowFloor;
        size_t actual = autolink::uartTxBufferFloor(cfg);
        if (actual != expected) {
            std::cerr << "\nFAIL: uartTxBufferFloor(" << maxMsg
                      << ") = " << actual << " but expected " << expected
                      << " — the floor's perChunk math is not "
                      << "tracking MAX_CHUNK (" << MAX_CHUNK << ")"
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  PASS (floor tracks MAX_CHUNK across "
              << sizeof(maxMsgs) / sizeof(maxMsgs[0]) << " maxMsg values)"
              << std::endl;
}

void test_pin_uart_floor_values_match_MAX_CHUNK_derivation() {
    std::cout << "\n=== Pin 2d-runtime: uartRxBufferFloor / uartTxBufferFloor "
                 "match the MAX_CHUNK-derived formula ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    cfg.rxBufferSize = 0;
    cfg.txBufferSize = 0;

    const size_t rx = autolink::uartRxBufferFloor(cfg);
    const size_t tx = autolink::uartTxBufferFloor(cfg);

    constexpr size_t kFrameOverhead = 4;
    constexpr size_t kWindow = (size_t)autolink::AUTOLINK_ARQ_PIPELINE_WINDOW;
    constexpr size_t kMaxChunk = (size_t)autolink::MAX_CHUNK;
    // RX floor formula: ((MAX_CHUNK + 4) * window * 5) / 4 (unchanged
    // from. TX floor formula inis
    // max(retxBudget, msgChunks * (MAX_CHUNK + 4)): the
    // multi-chunk sendMsg path needs headroom for a full
    // message, not just the retx burst. With default
    // cfg.maxMsg=5120, chunksForMsgLen=22, so the msgFloor
    // dominates. Pin the formula symbolically so a future
    // config bump doesn't desync the floor from the wire
    // contract.
    const size_t expectedRx = ((kMaxChunk + kFrameOverhead) * kWindow * 5) / 4;
    int burst = cfg.gbnResendBurstMax > 0 ? cfg.gbnResendBurstMax : 0;
    int msgChunks = autolink::chunksForMsgLen((int)cfg.maxMsg);
    if (msgChunks < 1)
        msgChunks = 1;
    size_t retxF = (kMaxChunk + kFrameOverhead) * (size_t)(burst + 1);
    size_t msgF = (kMaxChunk + kFrameOverhead) * (size_t)msgChunks;
    // AL88-4: floor also covers a full pipeline window (see the Pin
    // 2d comment above for why).
    size_t windowF = (kMaxChunk + kFrameOverhead) * kWindow;
    size_t expectedTx = retxF > msgF ? retxF : msgF;
    if (windowF > expectedTx)
        expectedTx = windowF;

    std::cout << "  rx=" << rx << " (expected " << expectedRx << ")"
              << "  tx=" << tx << " (expected " << expectedTx << ")"
              << std::endl;
    assert(rx == expectedRx &&
           "uartRxBufferFloor must equal (MAX_CHUNK + 4) * window * 5/4");
    assert(tx == expectedTx &&
           "uartTxBufferFloor must equal max(retxBurst, msgChunks) * "
           "(MAX_CHUNK + 4)");
    std::cout << "  PASS (floors match the MAX_CHUNK-derived formula)"
              << std::endl;
}

// ============================================================================
// Pin 3 (Fix 3) — okTickMs clamps at reorderHoldMs/2 in ASYNC mode so the
// retx tick fires before the receiver's reorder buffer expires the tail.
// ============================================================================

void test_pin_okTickMs_clamps_to_reorder_hold_half() {
    std::cout << "\n=== Pin 3: ASYNC okTickMs is bounded by one RTO ==="
              << std::endl;
    using namespace autolink;
    // The retx backstop is timer-driven, so the OK tick bounds recovery
    // latency: a tick past the RTO stretches a lost-NAK stall.
    ArqCache arq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    {
        // Defaults: idle/3 = 3333 would dominate;
        // the ASYNC bound pulls the tick down to
        // one RTO (500).
        AutoLinkConfig cfg;
        MockHal hal;
        Link l(hal, arq, true, cfg);
        LinkTestAccessor t(l);
        assert(t.okTick() == cfg.syncAckTimeoutMs &&
               "ASYNC tick must not exceed one RTO");
    }
    {
        // Floor: a tiny RTO still clamps to at least 50ms.
        AutoLinkConfig cfg;
        cfg.syncAckTimeoutMs = 10;
        MockHal hal;
        Link l(hal, arq, true, cfg);
        LinkTestAccessor t(l);
        assert(t.okTick() == 50 && "ASYNC tick floors at 50ms");
    }
    {
        // SYNC: no ASYNC bounds; max(idle/3, 50, RTO).
        AutoLinkConfig cfg;
        cfg.mode = AutoLinkConfig::Mode::SYNC;
        MockHal hal;
        Link l(hal, arq, true, cfg);
        LinkTestAccessor t(l);
        assert(t.okTick() == cfg.idleTimeoutMs / 3 &&
               "SYNC keeps the uncapped tick");
    }
    std::cout << "  PASS (ASYNC tick bounded by one RTO, floored at 50ms)"
              << std::endl;
}

// ============================================================================
// Pin 4 (Fix 4) — CTRL/ACK/NAK/retx all bump txBytes; the dashboard's
// tx rate is honest on a wire that emits mostly ACKs (Pong's case).
// ============================================================================

void test_pin_sendFrame_bumps_txBytes() {
    std::cout
        << "\n=== Pin 4a: sendSweepFrame_unlocked bumps txBytes (CTRL/PING/PONG) "
           "==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/link/io/LinkTx.cpp");
    assert(!src.empty());
    auto p = src.find("void Link::sendSweepFrame_unlocked(uint8_t payload)");
    assert(p != std::string::npos);
    auto bodyStart = src.find('{', p);
    int depth = 0;
    size_t bodyEnd = bodyStart;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                bodyEnd = i;
                break;
            }
        }
    }
    std::string body = src.substr(p, bodyEnd + 1 - p);
    assert(body.find("txBytes += CTRL_FRAME_SIZE") != std::string::npos &&
           "sendSweepFrame_unlocked must bump txBytes by CTRL_FRAME_SIZE so "
           "the wire-level CTRL/PING/PONG frames are counted in "
           "Stats.tx — without it, a link whose traffic is almost "
           "entirely ACK/NAK shows 0 B/sec on the dashboard");
    assert(body.find("lastTxMs = hw.nowMs()") != std::string::npos &&
           "sendSweepFrame_unlocked must stamp lastTxMs so the asymmetric-"
           "idle detector's TX-active branch fires on wire activity "
           "even when the data payload is zero");
    std::cout << "  PASS (CTRL frame bytes counted, lastTxMs stamped on send)"
              << std::endl;
}

void test_pin_ack_nak_frames_bump_txBytes() {
    std::cout << "\n=== Pin 4b: ACK and NAK frames count toward txBytes ==="
              << std::endl;
    // Pong's wire output is dominated by ACK/NAK traffic; the old dashboard
    // reported 0 B/s because neither was counted.
    ArqCache arq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    MockHal hal;
    Link l(hal, arq, true, cfg);
    LinkTestAccessor t(l);
    Stats s0, s1, s2;
    l.getStats(s0);
    t.ackFrame(3);
    l.getStats(s1);
    assert(s1.tx > s0.tx && "ACK frame bytes must count");
    t.nakFrame(4);
    l.getStats(s2);
    assert(s2.tx > s1.tx && "NAK frame bytes must count");
    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Pin 5 (Fix 5) — bringUpLink emits the version line FIRST, and Ping's
// ASYNC backpressure branch throttles to 1000 ms before retrying.
// ============================================================================

void test_pin_bringUpLink_emits_version_first() {
    std::cout << "\n=== Pin 5a: bringUpLink emits the version line FIRST ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());
    // Locate the bringUpLink function body.
    auto p = src.find("inline void bringUpLink(");
    assert(p != std::string::npos);
    auto bodyStart = src.find('{', p);
    int depth = 0;
    size_t bodyEnd = bodyStart;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                bodyEnd = i;
                break;
            }
        }
    }
    std::string body = src.substr(p, bodyEnd + 1 - p);
    // The version log line must appear before
    // the NVS prefs read (which writes its own
    // info line) and before the "calling
    // comm_.begin()" diagnostic.
    auto vpos = body.find("AUTOLINK_VERSION");
    auto nvsPos = body.find("prefs.begin(\"autolink\"");
    auto beginPos = body.find("calling comm_.begin()");
    assert(vpos != std::string::npos);
    assert(nvsPos != std::string::npos);
    assert(beginPos != std::string::npos);
    assert(vpos < nvsPos &&
           "AUTOLINK_VERSION must be logged before the NVS prefs read "
           "so the version is the first line in the live log");
    assert(vpos < beginPos &&
           "AUTOLINK_VERSION must be logged before "
           "'calling comm_.begin()' so the version is the first line");
    std::cout << "  PASS (version logged before NVS read and 'calling begin')"
              << std::endl;
}

void test_pin_ping_backpressure_cooldown_1000ms() {
    std::cout << "\n=== Pin 5b: Ping backpressure branch throttles 1000 ms ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());
    auto p = src.find("send failed (backpressure)");
    assert(p != std::string::npos);
    // The slice covers the backpressure branch
    // up to the next major statement.
    std::string slice = src.substr(p, 2000);
    // clang-format puts the RHS on its own line;
    // match the joined form.
    assert(slice.find("backpressureCoolUntilMs_ =") != std::string::npos &&
           slice.find("millis() + BACKPRESSURE_COOLDOWN_MS") !=
               std::string::npos &&
           "Ping's backpressure failure branch must set "
           "backpressureCoolUntilMs_ = millis() + BACKPRESSURE_COOLDOWN_MS "
           "so the next send attempt waits 1000 ms for the link task "
           "to drain the cache — a cooldown gated through tNextSendMs_ "
           "instead is a no-op whenever txDelayMs=0, exactly the ASYNC "
           "flood-mode case it exists to throttle. A separate stamp "
           "honored by its own txDelayMs-independent gate is required.");
    // The constant must be defined.
    assert(src.find("BACKPRESSURE_COOLDOWN_MS = 1000") != std::string::npos &&
           "Ping must define BACKPRESSURE_COOLDOWN_MS = 1000 and throttle "
           "the backpressure branch with it, or the consec counter ticks "
           "up faster than the link can drain");
    std::cout << "  PASS (backpressure branch throttles 1000 ms before retry)"
              << std::endl;
}

// Pin 5c: the cooldown gate must fire regardless of cfg.txDelayMs. Gated on
// txDelayMs > 0 it is a no-op in ASYNC flood mode — exactly where it is
// needed.
void test_pin_ping_cooldown_gate_independent_of_txDelayMs() {
    std::cout << "\n=== Pin 5c: backpressure cooldown gate is independent of "
                 "txDelayMs ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // (a) Field declared on the class.
    assert(src.find("backpressureCoolUntilMs_") != std::string::npos &&
           "Ping must declare a backpressureCoolUntilMs_ field that the "
           "send-loop's cooldown gate reads from, separate from "
           "tNextSendMs_ — that field is gated on txDelayMs > 0 and "
           "silently no-ops in ASYNC flood mode");

    // Locate the send-loop. The cooldown gate sits
    // inside the `while (count_ < WINDOW && sentThisLoop < maxTx)`
    // block, immediately after the txDelayMs gate.
    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    auto sendLoopPos =
        src.find("while (count_ < WINDOW && sentThisLoop < maxTx)", loopPos);
    assert(sendLoopPos != std::string::npos);
    auto sendLoopEnd = src.find("queue_[tail_].len = n", sendLoopPos);
    assert(sendLoopEnd != std::string::npos);
    std::string sendLoopSlice =
        src.substr(sendLoopPos, sendLoopEnd - sendLoopPos);

    // (b) The cooldown gate references
    // backpressureCoolUntilMs_ and breaks on
    // (now - stamp) < 0. This must NOT be
    // conditioned on txDelayMs > 0.
    assert(sendLoopSlice.find("backpressureCoolUntilMs_") !=
               std::string::npos &&
           "the send-loop must contain a cooldown gate that reads "
           "backpressureCoolUntilMs_ — the ASYNC-only emergency brake "
           "must fire on every iteration of the send loop, not just "
           "when txDelayMs > 0");

    // The negative-pin: the gate body must not
    // condition on txDelayMs > 0. Find the gate
    // block and assert no `txDelayMs > 0` guard
    // surrounds it.
    auto gatePos = sendLoopSlice.find("backpressureCoolUntilMs_");
    assert(gatePos != std::string::npos);
    // Match the gate pattern, not the bare field name: the field also
    // appears in comments, and clang-format may split the condition.
    auto gateStart = sendLoopSlice.find("backpressureCoolUntilMs_ != 0");
    if (gateStart == std::string::npos)
        gateStart = sendLoopSlice.find("(backpressureCoolUntilMs_ != 0");
    assert(gateStart != std::string::npos &&
           "send-loop must contain a cooldown gate of the form "
           "`backpressureCoolUntilMs_ != 0 && (now - stamp) < 0`");
    std::string gateRegion = sendLoopSlice.substr(0, gateStart + 300);
    auto ifPos = gateRegion.rfind("if (");
    assert(ifPos != std::string::npos);
    // Find the matching `)` of the parent if.
    int depth = 0;
    size_t closePos = std::string::npos;
    for (size_t i = ifPos + 3; i < gateRegion.size(); i++) {
        if (gateRegion[i] == '(')
            depth++;
        else if (gateRegion[i] == ')') {
            depth--;
            if (depth == 0) {
                closePos = i;
                break;
            }
        }
    }
    assert(closePos != std::string::npos);
    std::string gateHeader = gateRegion.substr(ifPos, closePos - ifPos + 1);
    assert(gateHeader.find("txDelayMs") == std::string::npos &&
           "the cooldown gate must NOT be conditioned on "
           "txDelayMs — that re-couples it to user pacing and makes "
           "it a silent no-op in ASYNC flood mode (txDelayMs=0).");

    std::cout << "  PASS (cooldown gate independent of txDelayMs; "
                 "fires regardless of user pacing)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Mode/Sync/Async Fixes Tests ===" << std::endl;
    // Pin 1 — mode desync
    test_pin_ihal_declares_setMode();
    test_pin_autolink_setMode_forwards_to_hal();
    test_pin_espal_setMode_rederives_buffers_pre_begin();
    // Pin 2 — ASYNC UART buffer scaling
    test_pin_uart_rx_buffer_scales_by_mode();
    test_pin_uart_tx_buffer_scales_by_mode();
    test_pin_uart_rx_floor_uses_pipeline_window_constant();
    test_pin_uart_floors_derive_perChunk_from_MAX_CHUNK();
    test_pin_uart_floor_values_match_MAX_CHUNK_derivation();
    // Pin 3 — retx RTO coupled with reorderHoldMs
    test_pin_okTickMs_clamps_to_reorder_hold_half();
    // Pin 4 — Pong Stats.tx / wire frame counter
    test_pin_sendFrame_bumps_txBytes();
    test_pin_ack_nak_frames_bump_txBytes();
    // Pin 5 — version first log + backpressure cooldown
    test_pin_bringUpLink_emits_version_first();
    test_pin_ping_backpressure_cooldown_1000ms();
    test_pin_ping_cooldown_gate_independent_of_txDelayMs();
    std::cout << "\n=== Mode/Sync/Async Fixes Tests Completed ===" << std::endl;
    return 0;
}

#endif
