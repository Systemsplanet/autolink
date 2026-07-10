// Source-grep + runtime regression test for the
// this-release ASYNC link-thrash fixes.
//
// Pre-this-release the bench logs showed
// `cfg.mode = ASYNC` at the Link layer but
// `cfg.mode = SYNC` at the HAL — the HAL
// sized UART / stream buffers and printed
// the boot log against SYNC while the link
// was running ASYNC. The 2 KB rx buffer
// overruns on a multi-chunk ASYNC flood,
// seq=58 dropped, the reorder buffer
// expired the tail before the sender's
// ~3.3 s OK-state tick could fire a retx,
// and the link cycle'd P1 -> P2 -> BREAK
// -> P1 indefinitely. The bench also showed
// `tx=0 B/sec` on Pong because ACK/NAK
// frames weren't counted in the Stats.tx
// field; and `Ping send failed
// (backpressure) ... consec=1` ran
// without any throttle, hammering the
// already-full cache.
//
// Five fixes are pinned here. Each pin
// fails when its fix is reverted; the
// suite as a whole is the regression wall
// for the bench symptoms.
//
//   1. IHal::setMode is the new public
//      hook; AutoLink::setMode forwards to
//      both the link and the HAL. EspHal
//      implements setMode and re-derives
//      the UART / stream-buffer floors
//      pre-begin(). Pre-begin-only: the
//      live UART buffers can't be resized
//      in flight (FreeRTOS stream-buffer
//      immutable-after-create), so a
//      post-begin setMode only updates
//      cfg.mode (so the boot log stays
//      honest) — mode switches still
//      require the NVS+reboot path.
//   2. uartRxBufferFloor / uartTxBufferFloor
//      scale the UART driver buffers by
//      mode (SYNC keeps the 2 KB/256 B
//      defaults; ASYNC uses a 32-slot
//      pipeline * 1.25x headroom).
//      streamBufferFloor already covered
//      the rx stream buffer (and stays
//      mode-independent — same
//      one-coalesced-message shape works
//      for both modes).
//   3. okTickMs() clamps at one RTO in
//      ASYNC mode so the OK-state timer
//      tick (which drives the GBN retx
//      sweep) fires promptly. SYNC mode is
//      unchanged — the sender blocks
//      inline for the receiver ACK and
//      never walks the ARQ table.
//   4. sendFrame_unlocked (PING/PONG/LCK
//      CTRL), sendAckFrame_unlocked,
//      sendCtrlCobsFrame_unlocked (NAK),
//      and resendCobsFrame_unlocked all
//      bump txBytes so the dashboard's
//      tx rate reflects the actual wire
//      output, not just data-payload
//      bytes. The data path through
//      buildAndSendMsg_unlocked was
//      already counted; Pong's wire
//      output is dominated by ACK/NAK
//      and pre-this-release reported
//      0 B/sec.
//   5. bringUpLink emits the version line
//      FIRST (before the NVS read and
//      before "calling comm_.begin()")
//      so the very first log line tells
//      the operator what firmware is
//      running. Ping's ASYNC backpressure
//      failure branch sets
//      tNextSendMs_ = millis() + 1000
//      before the next send attempt
//      so a single full-cache event
//      doesn't spin the send loop.
//
// All five pins are AGENTS rule 18
// compliant: toggling the fix off
// flips the matching pin red.
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
    // The pre-fix IHal had no setMode: AutoLink
    // forwarded to the link only, and EspHal held
    // a stale cfg copy sized off the ctor's
    // defaults. A NVS+reboot restore path that
    // set ASYNC before begin() never reached the
    // HAL, so the boot log printed SYNC and the
    // UART/stream buffer stayed SYNC-sized.
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
    // AutoLink owns both the HAL pointer and
    // the Link unique_ptr; the pre-fix setMode
    // called only `link->setMode(m)`. A future
    // refactor that drops the hal->setMode
    // forward flips this pin red.
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
           "(or alongside) link->setMode(m); pre-fix shape only called "
           "link, leaving the HAL's cfg copy stale");
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
    // Pre-begin setMode must re-derive the
    // UART and stream-buffer floors so a
    // NVS-restored ASYNC before begin() sizes
    // the buffers for that mode. Post-begin
    // setMode is a no-op (the buffers are
    // committed); the cfg.mode field is still
    // updated so the boot log stays honest.
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
           "from rxBufferFloor(cfg); pre-fix shape held a stale cfg "
           "copy and never re-sized");
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
    // ASYNC at default maxMsg=5120 with
    // AUTOLINK_ARQ_PIPELINE_WINDOW=32 needs at
    // least ~32 * 254 * 1.25 = 10160 bytes for
    // the rx driver buffer to hold a full
    // in-flight pipeline. The pre-fix shape
    // always used cfg.rxBufferSize (2048
    // default), which overruns on a multi-
    // chunk ASYNC flood.
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    const size_t asyncRx = autolink::uartRxBufferFloor(cfg);
    std::cout << "  ASYNC rx floor = " << asyncRx
              << " bytes (cfg.rxBufferSize=" << cfg.rxBufferSize << ")"
              << std::endl;
    assert(asyncRx > 4096 &&
           "ASYNC rx floor must be > 4 KB; the 32-slot pipeline alone is "
           "~8 KB. The pre-fix shape returned cfg.rxBufferSize=2048 "
           "regardless of mode, which underran on a multi-chunk flood");
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
    assert(syncTx == 256 &&
           "SYNC tx floor must return cfg.txBufferSize=256 unchanged");
    std::cout << "  PASS (caller's larger value wins, SYNC passes through)"
              << std::endl;
}

void test_pin_uart_rx_floor_uses_pipeline_window_constant() {
    std::cout << "\n=== Pin 2c: uartRxBufferFloor scales with the ARQ pipeline "
                 "window ==="
              << std::endl;
    // The rx floor must use the same
    // AUTOLINK_ARQ_PIPELINE_WINDOW constant
    // the ARQ cache uses, so the two
    // sizes stay in lockstep. A future
    // bump to the window bumps the rx
    // floor in lockstep.
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

// ============================================================================
// Pin 2d — UART buffer floors derive perChunk from MAX_CHUNK symbolically,
// not from a literal kChunkCap=250. A future MAX_CHUNK bump would silently
// desync the floor math from the wire-protocol chunk cap the framer honors,
// so the pin enforces that the symbolic reference is present and the
// literal is gone.
// ============================================================================

void test_pin_uart_floors_derive_perChunk_from_MAX_CHUNK() {
    std::cout << "\n=== Pin 2d: UART floors derive perChunk from MAX_CHUNK "
                 "(no literal kChunkCap) ==="
              << std::endl;
    // Source-grep pin. The pre-fix
    // shape had `constexpr int
    // kChunkCap = 250;` inside both
    // uartRxBufferFloor and
    // uartTxBufferFloor. A MAX_CHUNK
    // bump would leave the floor
    // functions reading the old literal
    // while the framer and the seq-space
    // math both tracked the new value
    // — silent wire vs. buffer
    // desync. The fix removes the
    // literal and references MAX_CHUNK
    // symbolically (it lives in
    // al/link/LinkWire.h, included
    // at the top of AutoLinkConfig.h).
    std::string src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!src.empty());

    // (a) The literal must be gone from
    //     the floor definitions.
    assert(src.find("kChunkCap = 250") == std::string::npos &&
           "uartRxBufferFloor / uartTxBufferFloor must not carry a "
           "literal kChunkCap=250 — that drifts from MAX_CHUNK the "
           "moment the chunk cap is bumped in LinkWire.h");

    // (b) The floor bodies must
    //     reference MAX_CHUNK
    //     symbolically. We look for the
    //     pattern inside the two floor
    //     function bodies (each defined
    //     once, no forward decl).
    auto pRx = src.find("inline size_t uartRxBufferFloor");
    auto pTx = src.find("inline size_t uartTxBufferFloor");
    assert(pRx != std::string::npos && pTx != std::string::npos);
    // Read up to 800 chars per body —
    // enough to cover the perChunk math
    // without crossing into the next
    // function.
    std::string rxBody = src.substr(pRx, 800);
    std::string txBody = src.substr(pTx, 800);
    assert(rxBody.find("MAX_CHUNK") != std::string::npos &&
           "uartRxBufferFloor must reference MAX_CHUNK symbolically "
           "(from al/link/LinkWire.h) so a chunk-cap bump can't "
           "desync the floor from the framer");
    assert(txBody.find("MAX_CHUNK") != std::string::npos &&
           "uartTxBufferFloor must reference MAX_CHUNK symbolically "
           "(from al/link/LinkWire.h)");

    // (c) chunksForMsgLen must also
    //     reference MAX_CHUNK and MSG_HDR
    //     symbolically (the same drift
    //     class — a literal `+ 250` /
    //     `+ 6` mirror would silently
    //     desync the seq-space guard from
    //     the wire cap). The formula
    //     uses `len + MSG_HDR <= MAX_CHUNK`
    //     and `(len + MAX_CHUNK - 1) /
    //     MAX_CHUNK`; we assert both
    //     named constants appear in the
    //     body and the bare-literal
    //     arithmetic forms are gone.
    auto pChunks = src.find("constexpr int chunksForMsgLen");
    assert(pChunks != std::string::npos);
    std::string chunksBody = src.substr(pChunks, 800);
    assert(chunksBody.find("MAX_CHUNK") != std::string::npos &&
           "chunksForMsgLen must reference MAX_CHUNK symbolically");
    assert(chunksBody.find("MSG_HDR") != std::string::npos &&
           "chunksForMsgLen must reference MSG_HDR symbolically");
    assert(chunksBody.find("kChunkCap = 250") == std::string::npos &&
           "chunksForMsgLen must not carry a literal kChunkCap=250");
    // Strip C++ comments so a comment
    // containing the constant name
    // doesn't fake the formula check.
    auto stripComments = [](std::string s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n')
                    i++;
            } else {
                out += s[i++];
            }
        }
        return out;
    };
    std::string chunksCode = stripComments(chunksBody);
    assert(chunksCode.find("len + MSG_HDR <= MAX_CHUNK") != std::string::npos &&
           "chunksForMsgLen's coalesce-cap condition must use "
           "len + MSG_HDR <= MAX_CHUNK symbolically — the pre-fix "
           "mirror `len + 6 <= 250` silently desyncs from a chunk-cap "
           "or hdr-length bump");
    assert(chunksCode.find("(len + MAX_CHUNK - 1) / MAX_CHUNK") !=
               std::string::npos &&
           "chunksForMsgLen's chunk-count math must use "
           "(len + MAX_CHUNK - 1) / MAX_CHUNK — the pre-fix mirror "
           "`(len + 249) / 250` silently desyncs from a chunk-cap bump");

    std::cout << "  PASS (floor + chunksForMsgLen derive from MAX_CHUNK)"
              << std::endl;
}

void test_pin_uart_floor_values_match_MAX_CHUNK_derivation() {
    std::cout << "\n=== Pin 2d-runtime: uartRxBufferFloor / uartTxBufferFloor "
                 "match the MAX_CHUNK-derived formula ==="
              << std::endl;
    // Runtime check that the new
    // symbolic derivation produces
    // exactly the same numbers the
    // old literal would have, given
    // MAX_CHUNK = 250. If a future
    // MAX_CHUNK bump lands, the floor
    // shifts in lockstep — the values
    // here change too, which is the
    // point.
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    cfg.rxBufferSize = 0;
    cfg.txBufferSize = 0;

    const size_t rx = autolink::uartRxBufferFloor(cfg);
    const size_t tx = autolink::uartTxBufferFloor(cfg);

    constexpr size_t kFrameOverhead = 4;
    constexpr size_t kWindow = (size_t)autolink::AUTOLINK_ARQ_PIPELINE_WINDOW;
    constexpr size_t kMaxChunk = (size_t)autolink::MAX_CHUNK;
    const size_t expectedRx = ((kMaxChunk + kFrameOverhead) * kWindow * 5) / 4;
    const size_t expectedTx = ((kMaxChunk + kFrameOverhead) * 3) / 2;

    std::cout << "  rx=" << rx << " (expected " << expectedRx << ")"
              << "  tx=" << tx << " (expected " << expectedTx << ")"
              << std::endl;
    assert(rx == expectedRx &&
           "uartRxBufferFloor must equal (MAX_CHUNK + 4) * window * 5/4");
    assert(tx == expectedTx &&
           "uartTxBufferFloor must equal (MAX_CHUNK + 4) * 3/2");
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
    // The GBN retx backstop is timer-driven, so the OK tick
    // bounds recovery latency: a tick past the RTO stretches
    // a lost-NAK stall. There's no reorder hold left to race
    // (out-of-order frames are dropped, not buffered) — the
    // ASYNC bound is exactly one RTO. SYNC never walks the
    // ARQ table and keeps the uncapped max(idle/3, 50, RTO)
    // shape.
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
        << "\n=== Pin 4a: sendFrame_unlocked bumps txBytes (CTRL/PING/PONG) "
           "==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/link/LinkTx.cpp");
    assert(!src.empty());
    auto p = src.find("void Link::sendFrame_unlocked(uint8_t payload)");
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
           "sendFrame_unlocked must bump txBytes by CTRL_FRAME_SIZE so "
           "the wire-level CTRL/PING/PONG frames are counted in "
           "Stats.tx. The pre-fix shape didn't bump txBytes here, so "
           "Pong's dashboard tx rate stayed at 0 B/sec");
    assert(body.find("lastTxMs = hw.nowMs()") != std::string::npos &&
           "sendFrame_unlocked must stamp lastTxMs so the asymmetric-"
           "idle detector's TX-active branch fires on wire activity "
           "even when the data payload is zero");
    std::cout << "  PASS (CTRL frame bytes counted, lastTxMs stamped on send)"
              << std::endl;
}

void test_pin_ack_nak_frames_bump_txBytes() {
    std::cout << "\n=== Pin 4b: ACK and NAK frames count toward txBytes ==="
              << std::endl;
    // Pong's wire output is dominated by ACK/NAK traffic; the pre-fix
    // dashboard reported 0 B/s because neither was counted.
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
    // The backpressure-failure branch must set
    // backpressureCoolUntilMs_ to millis() + 1000
    // BEFORE the loop exits. Find the
    // "send failed (backpressure)" log site (the
    // backpressure branch's first action after the
    // link-not-OK early-return) and the
    // BACKPRESSURE_COOLDOWN_MS assignment.
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
           "to drain the cache. The pre-fix shape wrote "
           "tNextSendMs_, but the send-loop's txDelayMs gate only "
           "honored tNextSendMs_ when txDelayMs > 0 — ASYNC flood "
           "mode (txDelayMs=0, the exact bench scenario that "
           "produced the consec=1378 storm) skipped the throttle "
           "entirely. A separate stamp honored by its own "
           "txDelayMs-independent gate is the fix.");
    // The constant must be defined.
    assert(src.find("BACKPRESSURE_COOLDOWN_MS = 1000") != std::string::npos &&
           "Ping must define BACKPRESSURE_COOLDOWN_MS = 1000; the "
           "pre-fix shape didn't throttle the backpressure branch at "
           "all, and the bench log showed the consec counter ticking "
           "up faster than the link could drain");
    std::cout << "  PASS (backpressure branch throttles 1000 ms before retry)"
              << std::endl;
}

// Pin 5c: the cooldown stamp must be honored by
// a send-loop gate that fires regardless of
// cfg.txDelayMs. The pre-fix shape honored
// only tNextSendMs_ (and only when txDelayMs > 0),
// so the cooldown was a no-op in ASYNC flood mode
// — the exact scenario that produced the
// consec=1378 storm on the bench. This pin
// asserts (a) the field is declared on the class,
// (b) the send-loop contains a gate on the field
// that does NOT condition on txDelayMs > 0, and
// (c) the negative-pin set excludes txDelayMs from
// the cooldown gate so a future regression that
// re-couples them flips red.
void test_pin_ping_cooldown_gate_independent_of_txDelayMs() {
    std::cout << "\n=== Pin 5c: backpressure cooldown gate is independent of "
                 "txDelayMs ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // (a) Field declared on the class.
    assert(src.find("backpressureCoolUntilMs_") != std::string::npos &&
           "Ping must declare a backpressureCoolUntilMs_ field that the "
           "send-loop's cooldown gate reads from; the pre-fix shape "
           "abused tNextSendMs_ for this, which is gated on "
           "txDelayMs > 0 and silently no-ops in ASYNC flood mode");

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
    // The first `backpressureCoolUntilMs_` in the
    // send-loop slice may be inside a comment.
    // Find the gate by looking for the gate pattern
    // itself — `if (backpressureCoolUntilMs_ != 0 &&`
    // or `(backpressureCoolUntilMs_ != 0 &&` (in
    // case clang-format broke it across lines).
    auto gateStart = sendLoopSlice.find("backpressureCoolUntilMs_ != 0");
    if (gateStart == std::string::npos)
        gateStart = sendLoopSlice.find("(backpressureCoolUntilMs_ != 0");
    assert(gateStart != std::string::npos &&
           "send-loop must contain a cooldown gate of the form "
           "`backpressureCoolUntilMs_ != 0 && (now - stamp) < 0`");
    // Find the gate's parent `if (...)`. Walk back
    // from gateStart until we find `if (`. Include
    // enough forward context that the matching
    // closing paren (which may sit on a continuation
    // line after clang-format) is reachable.
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
    // The cooldown gate's parent-if header must NOT
    // mention txDelayMs — if it does, the throttle
    // is gated on user pacing and silently no-ops
    // when txDelayMs=0 (the ASYNC flood mode that
    // produced the bench storm).
    assert(gateHeader.find("txDelayMs") == std::string::npos &&
           "the cooldown gate must NOT be conditioned on "
           "txDelayMs — that re-couples it to user pacing "
           "and silently no-ops in ASYNC flood mode (the "
           "exact scenario this fix targets). The "
           "pre-fix shape conditioned on "
           "txDelayMs > 0, which made the cooldown "
           "a no-op in ASYNC flood mode.");

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
