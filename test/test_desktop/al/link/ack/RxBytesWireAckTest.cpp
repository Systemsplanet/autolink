// Runtime + structural pin for the inbound wire ACK/NAK/CTRL byte
// counter. rxBytes must bump on onAck and onNak, not only on the
// app-payload path (onPayload -> pushAppBuf -> rxBytes += n): in
// the PingPong protocol Pong replies with a wire-level ACK frame,
// dispatched to onAck, never onPayload, so a sender counting only
// the payload path shows a permanently-zero RX total against a
// healthy non-zero TX total on the peer. RX_ACK_WIRE_BYTES=8 and
// RX_NAK_WIRE_BYTES=6 are the COBS+framing totals for the 5-byte
// ACK and 3-byte NAK raw frames, declared in LinkWire.h alongside
// MAX_CHUNK.
//
// Pin set:
//
//   1. Link::onAck bumps rxBytes by
//      RX_ACK_WIRE_BYTES (8) per inbound ACK
//      frame. A ping that only receives wire
//      ACKs (Pong does NOT echo the payload
//      back in this protocol) sees rxBytes
//      advance on every ACK arrival.
//   2. Link::onNak bumps rxBytes by
//      RX_NAK_WIRE_BYTES (6) per inbound NAK
//      frame. A peer that sends a NAK on a gap
//      contributes its wire bytes to the
//      receiver's RX total.
//   3. RX_ACK_WIRE_BYTES / RX_NAK_WIRE_BYTES
//      constants live in LinkWire.h (single
//      source of truth alongside MAX_CHUNK and
//      MSG_HDR), and the link layer's onAck /
//      onNak reference them by name. A future
//      ACK frame-shape change (e.g. 7-byte
//      ACK with timestamp) only has to update
//      the constant in one place; the rx-byte
//      count follows.
//   4. The ping's RX total post-flood exceeds
//      its pre-flood RX total even though the
//      ping never delivers an app payload to
//      recvMsg() (the wire-ACK reply path
//      doesn't push to the app buffer).
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>
#include "MockHal.h"
#include "WireSim.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/link/LinkWire.h"
#include "al/link/io/LinkFrameRx.h"
#include "al/util/codec/UtilCobs.h"
#include "al/util/codec/UtilCrc.h"

using namespace autolink;

namespace {

void driveToOk(Link &ping, Link &pong, MockHal &mHal, MockHal &sHal) {
    ping.begin();
    pong.begin();
    while (ping.getState() != State::OK || pong.getState() != State::OK) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
}

// Pin 1: onAck bumps rxBytes by RX_ACK_WIRE_BYTES
// per inbound ACK. Mark seq as pending first
// (onAck early-returns when the seq isn't in the
// pending table), then feed a synthetic ACK
// frame and verify rxBytes advanced by exactly
// RX_ACK_WIRE_BYTES. The bump must happen EVEN
// when isPending is true (the real Ping-side
// scenario after a successful send) — original didn't bump at all.
void test_on_ack_bumps_rx_bytes() {
    std::cout << "\n=== Pin 1: onAck bumps rxBytes by RX_ACK_WIRE_BYTES ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::SYNC;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    driveToOk(ping, pong, mHal, sHal);
    // Age past the post-lock wire-settle window: onAck (like
    // onPayload/onNak) silently drops frames arriving within
    // AUTOLINK_WIRE_SETTLE_MS of the lock, so an injection
    // immediately after driveToOk never reaches the accounting
    // this pin measures. Red since the settle gate landed; the
    // contract itself is intact once past settle.
    for (int agei = 0; agei < 6; agei++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }

    // Mark seq 0x01 as pending so onAck proceeds
    // past the isPending short-circuit.
    LinkTestAccessor pingT(ping);
    pingT.markAckedPending(0x01);

    Stats pre;
    ping.getStats(pre);

    // Inject a synthetic wire-ACK frame into
    // ping's onRx path. Use LinkTestAccessor to
    // reach the UtilFrameRx::feed helper.
    //
    // 5-byte raw ACK: [0xFF, seq, bytes_lo, bytes_hi, crc8]
    uint8_t ackRaw[5] = { 0xFF, 0x01, 0x10, 0x00, 0 };
    ackRaw[4] = UtilCrc::crc8(ackRaw, 4);
    uint8_t wireBuf[16];
    wireBuf[0] = 0x00;
    size_t encLen = UtilCobs::encode(ackRaw, 5, wireBuf + 1);
    wireBuf[1 + encLen] = 0x00;

    // Feed ping's UtilFrameRx directly (ping IS
    // its own Listener via private inheritance).
    (void)pingT.utilFrameRxFeed(wireBuf, (int)(encLen + 2));

    Stats post;
    ping.getStats(post);
    int64_t delta = (int64_t)post.rx - (int64_t)pre.rx;
    assert(delta == RX_ACK_WIRE_BYTES &&
           "onAck must bump rxBytes by RX_ACK_WIRE_BYTES; the old "
           "shape only counted bytes on the app-payload path, so a "
           "ping that only received wire ACKs saw rxBytes stuck at 0");
    std::cout << "  PASS (onAck: rxBytes += " << delta
              << " (= " << RX_ACK_WIRE_BYTES << "))" << std::endl;
}

// Pin 2: onNak bumps rxBytes by RX_NAK_WIRE_BYTES.
// Same shape as Pin 1 but with a NAK frame.
void test_on_nak_bumps_rx_bytes() {
    std::cout << "\n=== Pin 2: onNak bumps rxBytes by RX_NAK_WIRE_BYTES ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::SYNC;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    driveToOk(ping, pong, mHal, sHal);
    // Age past the post-lock wire-settle window: onAck (like
    // onPayload/onNak) silently drops frames arriving within
    // AUTOLINK_WIRE_SETTLE_MS of the lock, so an injection
    // immediately after driveToOk never reaches the accounting
    // this pin measures. Red since the settle gate landed; the
    // contract itself is intact once past settle.
    for (int agei = 0; agei < 6; agei++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }

    LinkTestAccessor pingT(ping);
    pingT.markAckedPending(0x01);

    Stats pre;
    ping.getStats(pre);

    // 3-byte raw NAK: [0xFE, seq, crc8]
    uint8_t nakRaw[3] = { 0xFE, 0x01, 0 };
    nakRaw[2] = UtilCrc::crc8(nakRaw, 2);
    uint8_t wireBuf[16];
    wireBuf[0] = 0x00;
    size_t encLen = UtilCobs::encode(nakRaw, 3, wireBuf + 1);
    wireBuf[1 + encLen] = 0x00;

    (void)pingT.utilFrameRxFeed(wireBuf, (int)(encLen + 2));

    Stats post;
    ping.getStats(post);
    int64_t delta = (int64_t)post.rx - (int64_t)pre.rx;
    assert(delta == RX_NAK_WIRE_BYTES &&
           "onNak must bump rxBytes by RX_NAK_WIRE_BYTES");
    std::cout << "  PASS (onNak: rxBytes += " << delta
              << " (= " << RX_NAK_WIRE_BYTES << "))" << std::endl;
}

// Pin 3: structural. LinkRx.cpp references the
// RX_ACK_WIRE_BYTES / RX_NAK_WIRE_BYTES constants
// by name, and LinkWire.h defines them. A
// future wire-frame-shape change that needs a
// different ACK or NAK size has only one place
// to update (LinkWire.h).
void test_constants_defined_and_referenced() {
    std::cout << "\n=== Pin 3: RX_*_WIRE_BYTES constants in LinkWire.h, "
                 "referenced by name in LinkRx.cpp ==="
              << std::endl;
    // Definition in LinkWire.h.
    assert(RX_ACK_WIRE_BYTES == 8 &&
           "RX_ACK_WIRE_BYTES must be 8 (5 raw ACK COBS-encodes to 6, "
           "wire adds 2 framing = 8)");
    assert(RX_NAK_WIRE_BYTES == 6 &&
           "RX_NAK_WIRE_BYTES must be 6 (3 raw NAK COBS-encodes to 4, "
           "wire adds 2 framing = 6)");

    // Find the project root by walking up looking for AGENTS.md.
    std::string base = ".";
    std::string root = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good()) {
            root = base;
            break;
        }
        base += "/..";
    }
    std::ifstream f(root + "/src/al/link/io/LinkRx.cpp");
    assert(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    std::string src = ss.str();
    assert(src.find("RX_ACK_WIRE_BYTES") != std::string::npos &&
           "LinkRx.cpp must reference RX_ACK_WIRE_BYTES by name");
    assert(src.find("RX_NAK_WIRE_BYTES") != std::string::npos &&
           "LinkRx.cpp must reference RX_NAK_WIRE_BYTES by name");
    std::cout << "  PASS (constants defined in LinkWire.h; "
                 "LinkRx.cpp references by name)"
              << std::endl;
}

// Pin 4: end-to-end — a pong that emits wire
// ACKs in response to a ping's sends bumps the
// ping's rxBytes. Uses the WireSim loopback
// fixture which handles the SYNC-mode inline
// ACK round-trip and the two-node pipe data.
void test_wire_ack_only_path_bumps_rx() {
    std::cout << "\n=== Pin 4: wire-ACK-only reply path bumps rxBytes "
                 "(Pong doesn't echo payload) ==="
              << std::endl;
    WireSim sim;
    TwoNodeFixture fx(sim);
    fx.begin();

    // Run the fixture for a bit to drive the
    // pong side into OK.
    for (int i = 0; i < 50 && sim.getStateA() != State::OK; i++)
        fx.step(20);

    AutoLink &ping = sim.linkA();

    // Pong-side recv() returns the payload Ping
    // sent. PongNode then drops it (no echo back).
    // The ping's rx counter must still advance
    // because PongNode emits a wire-ACK for each
    // received chunk.
    Stats pre;
    ping.getStats(pre);

    // Pump the fixture — it drives Ping→Pong
    // sends and Pong recv-side drains; Pong's
    // ACK frames flow back on the wire to Ping.
    for (int i = 0; i < 50; i++)
        fx.step(20);

    Stats post;
    ping.getStats(post);
    int64_t delta = (int64_t)post.rx - (int64_t)pre.rx;
    assert(delta > 0 &&
           "ping.rxBytes must advance after Pong's wire-ACK frames — "
           "a counter that only bumps on the app-payload path stays "
           "at 0 forever whenever the peer's reply is wire-ACK-only");
    std::cout << "  PASS (wire-ACK-only reply path: ping.rxBytes += " << delta
              << " > 0)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running RX Bytes Wire-ACK Tests ===" << std::endl;
    test_on_ack_bumps_rx_bytes();
    test_on_nak_bumps_rx_bytes();
    test_constants_defined_and_referenced();
    test_wire_ack_only_path_bumps_rx();
    std::cout << "\n=== RX Bytes Wire-ACK Tests PASS ===" << std::endl;
    return 0;
}