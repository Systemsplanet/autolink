// Source-level regression test for the current shape
// post-lock settle gate. The buggy-original shape
// transitioned SWP -> OK and immediately
// accepted COBS / CTRL / ACK / NAK frames
// arriving on the new baud. The baud switch
// (setSpd / uart_set_baudrate) leaves line
// garbage in the rx FIFO and any in-flight
// frames from the prior baud; both race with
// the new session's frame parser. The field
// log of the run A wedge shows NAKs at
// 17.983-17.995 (frame-arrival time) firing
// BEFORE the app-side settle drain at 18.018
// — the link layer was NAKing during the
// settle window, on noise frames.
//
// The fix:
//   1. lockOk_unlocked() drains the app buf
//      and the UART FIFO at lock time.
//   2. lockOk_unlocked() opens a settle
//      window (settleUntilMs_ = now +
//      AUTOLINK_APP_SETTLE_MS) during which
//      every wire path (onPayload, onAck,
//      onNak, processCtrlFrame_unlocked)
//      drops the frame silently — no ACK,
//      no NAK, no app-buf write, no rxSeq
//      advance. The peer's RTO will
//      re-transmit, by which time the
//      settle window has closed.
//
// This test pins:
//   a. lockOk_unlocked() drains the app
//      buffer (calls hw.clearAppBuf()).
//   b. lockOk_unlocked() drains the UART
//      FIFO (calls hw.flushRxHw()).
//   c. lockOk_unlocked() opens a settle
//      window via settleUntilMs_.
//   d. onPayload, onAck, onNak, and
//      processCtrlFrame_unlocked each
//      have a settle-guard that drops
//      frames arriving before
//      settleUntilMs_ elapses.
//   e. The settle guard is the FIRST thing
//      in each function body (before the
//      CRC check, before any state advance)
//      so a pre-settle noise frame can't
//      trigger err_unlocked() or any state
//      mutation.

#ifndef ARDUINO

#    include <cassert>
#    include <cstdint>
#    include <cstdio>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <vector>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "LinkTestAccessor.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "al/AutoLinkConfig.h"
#    include "al/util/log/Log.h"
#    include "al/util/codec/UtilCrc.h"

using namespace autolink;

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return std::string();
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
    return std::string();
}

std::string extractFnBody(const std::string &src,
                          const std::string &signature) {
    auto start = src.find(signature);
    if (start == std::string::npos)
        return std::string();
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return std::string();
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(start, i + 1 - start);
        }
    }
    return std::string();
}

// Pin a: lockOk_unlocked() calls clearAppBuf
// (drains the app buf at lock time).
void test_lockOk_drains_app_buf() {
    std::cout
        << "\n=== Pin a: lockOk_unlocked drains the app buf at lock time ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/timers/LinkSweepGlue.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void Link::lockOk_unlocked");
    assert(!body.empty());
    auto p = body.find("hw.clearAppBuf()");
    assert(p != std::string::npos);
    std::cout << "  hw.clearAppBuf() at offset " << p
              << " in lockOk_unlocked \u2713" << std::endl;
    std::cout << "  PASS (app-buf drained at lock time)" << std::endl;
}

// Pin b: lockOk_unlocked() calls flushRxHw
// (drains the UART FIFO at lock time).
void test_lockOk_drains_uart_fifo() {
    std::cout << "\n=== Pin b: lockOk_unlocked drains the UART FIFO ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/timers/LinkSweepGlue.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void Link::lockOk_unlocked");
    assert(!body.empty());
    auto p = body.find("hw.flushRxHw()");
    assert(p != std::string::npos);
    std::cout << "  hw.flushRxHw() at offset " << p
              << " in lockOk_unlocked \u2713" << std::endl;
    std::cout << "  PASS (UART FIFO drained at lock time)" << std::endl;
}

// Pin c: lockOk_unlocked() opens a settle
// window via settleUntilMs_.
void test_lockOk_opens_settle_window() {
    std::cout << "\n=== Pin c: lockOk_unlocked opens a settle window ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/timers/LinkSweepGlue.cpp");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void Link::lockOk_unlocked");
    assert(!body.empty());
    auto p = body.find("settleUntilMs_ = hw.nowMs() + AUTOLINK_WIRE_SETTLE_MS");
    assert(p != std::string::npos);
    std::cout
        << "  settleUntilMs_ set to hw.nowMs() + AUTOLINK_WIRE_SETTLE_MS at "
           "offset "
        << p << " \u2713" << std::endl;
    std::cout << "  PASS (settle window opened at lock time)" << std::endl;
}

// Pin d: the settle window gates on VALIDATION,
// not on arrival. A CRC-valid CTRL frame that
// lands inside the window is processed normally
// (the peer gets its PONG-ack); a CRC-failed one
// is swallowed without counting a frame error and
// is counted in Stats.settleDrops instead.
//
// The old contract dropped everything that arrived
// inside the window, valid frames included. That
// silently discarded proven-good data: the sender
// got no ACK and no NAK, ran its RTO ladder into
// the base-stuck monitor, and took an honest link
// drop while the receiver was deliberately
// throwing the data away. Every chunk of a message
// sent in the first AUTOLINK_WIRE_SETTLE_MS after a
// lock was lost that way, with nothing counted
// anywhere.
void test_valid_frame_survives_settle_window() {
    std::cout << "\n=== Pin d: a CRC-valid frame inside the settle window "
                 "is processed, not dropped ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, aArq, true, cfg);
    Link pong(sHal, bArq, false, cfg);
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK && pong.getState() == State::OK);

    // lockPair returns the instant both sides read OK, so we are
    // inside the settle window by construction. Assert that
    // rather than assume it.
    assert(LinkTestAccessor(pong).settleWindowOpenForTest() &&
           "test must run inside the settle window");

    sHal.clearTx();
    mHal.clearTx();

    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = LinkTestAccessor(pong).peerSweepEpochForTest();
    frame[CTRL_FRAME_PAYLOAD_IDX] = PING_CMD;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);

    Stats base;
    pong.getStats(base);
    pong.onRx(frame, CTRL_FRAME_SIZE);
    Stats after;
    pong.getStats(after);

    bool gotPongAck = sHal.txBuf.size() >= CTRL_FRAME_SIZE &&
        sHal.txBuf[0] == 0xAA && sHal.txBuf[1] == 0x55 &&
        sHal.txBuf[CTRL_FRAME_PAYLOAD_IDX] == PONG_CMD;
    assert(gotPongAck &&
           "CRC-valid PING inside the settle window must be "
           "answered, not dropped");
    assert(after.frameErrs == base.frameErrs);
    assert(after.settleDrops == base.settleDrops);
    std::cout << "  CRC-valid PING inside the window -> PONG-ack on the wire, "
                 "frameErrs and settleDrops unchanged \u2713"
              << std::endl;
    std::cout << "  PASS (validation gates the window, arrival does not)"
              << std::endl;
}

// Pin e: garbage inside the window is rejected
// without being counted as a link fault and
// without being answered. In OK state the CTRL
// scan's CRC8 pre-check catches it before the
// window is ever consulted — the window only has
// to swallow anything in SWP, and when it does it
// increments Stats.settleDrops so the swallow is
// never silent. Either way frameErrs must not
// move: baud-switch re-frames are expected, not
// a fault.
void test_garbage_inside_settle_window_is_swallowed_and_counted() {
    std::cout << "\n=== Pin e: CRC-failed garbage inside the window is "
                 "swallowed without a frame error, and counted ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, aArq, true, cfg);
    Link pong(sHal, bArq, false, cfg);
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK && pong.getState() == State::OK);
    assert(LinkTestAccessor(pong).settleWindowOpenForTest() &&
           "test must run inside the settle window");

    sHal.clearTx();
    mHal.clearTx();

    // Same preamble, deliberately wrong CRC — exactly the shape
    // of a baud-switch re-frame.
    uint8_t garbage[CTRL_FRAME_SIZE];
    garbage[0] = 0xAA;
    garbage[1] = 0x55;
    garbage[CTRL_FRAME_SEQ_IDX] = 0x7E;
    garbage[CTRL_FRAME_PAYLOAD_IDX] = PING_CMD;
    garbage[CTRL_FRAME_CRC_IDX] =
        (uint8_t)(UtilCrc::crc8(garbage, CTRL_FRAME_SIZE - 1) ^ 0xFF);

    Stats base;
    pong.getStats(base);
    pong.onRx(garbage, CTRL_FRAME_SIZE);
    Stats after;
    pong.getStats(after);

    assert(after.frameErrs == base.frameErrs &&
           "baud-switch garbage inside the window is expected, not a fault");
    assert(sHal.txBuf.empty() && "garbage must not be answered");
    // Rejected by the OK-state CRC pre-check, so the window
    // itself never saw it. Any nonzero delta here would mean
    // the pre-check regressed and the window is doing the
    // catching — still safe, but the layering changed.
    assert(after.settleDrops == base.settleDrops &&
           "in OK state the CRC pre-check rejects garbage before the "
           "settle window is consulted");
    std::cout << "  CRC-failed frame inside the window -> rejected, "
                 "frameErrs unchanged, no answer on the wire \u2713"
              << std::endl;
    std::cout << "  PASS (garbage is rejected without being called a fault)"
              << std::endl;
}

// Pin f: the wire-settle constant is small
// enough that the linklayer-loopback test
// can still exchange data within its
// 2s smoke window. A 600ms wire gate
// (re-using the app-side AUTOLINK_APP_SETTLE_MS)
// wedges the loopback because the GBN window
// fills with chunks the receiver drops
// silently, and the retx wave arrives inside
// the gate and is dropped again. The loopback
// expects first RX within 1.2s of OK, so the
// wire gate must be < ~300ms (an order of
// magnitude over the observed 35ms drain gap,
// with margin for the test's 100ms clock tick).
// Pinned by the loopback smoke; a future
// bump to AUTOLINK_WIRE_SETTLE_MS > 300 will
// flip this test RED.
void test_wire_settle_constant_is_short() {
    std::cout
        << "\n=== Pin f: AUTOLINK_WIRE_SETTLE_MS stays under the loopback's 1.2s RX window ==="
        << std::endl;
    std::string root = projectRoot();
    std::string cfg = readFile(root + "/src/al/AutoLinkConfig.h");
    assert(!cfg.empty());
    auto p = cfg.find("AUTOLINK_WIRE_SETTLE_MS =");
    assert(p != std::string::npos);
    auto v = cfg.find_first_of("0123456789", p);
    assert(v != std::string::npos);
    uint32_t ms = (uint32_t)std::stoul(cfg.substr(v));
    std::cout << "  AUTOLINK_WIRE_SETTLE_MS = " << ms
              << "ms (<= 300ms keeps the loopback smoke green) \u2713"
              << std::endl;
    assert(ms <= 300);
    std::cout
        << "  PASS (wire-settle constant fits inside the loopback RX window)"
        << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Settle-gate regression tests ===" << std::endl;
    test_lockOk_drains_app_buf();
    test_lockOk_drains_uart_fifo();
    test_lockOk_opens_settle_window();
    test_valid_frame_survives_settle_window();
    test_garbage_inside_settle_window_is_swallowed_and_counted();
    test_wire_settle_constant_is_short();
    std::cout << "\n=== Settle-gate tests PASS ===" << std::endl;
    return 0;
}

#endif
