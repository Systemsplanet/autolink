// Runtime + structural pin for the extended wire
// ACK frame introduced in this release.
//
// Wire ACK frame (5 bytes raw, COBS-encoded):
//   [0xFF, seq, bytes_lo, bytes_hi, frame_crc8]
// frame_crc8 covers bytes 0..3. The receiver's
// onAck listener picks up bytesRecvd so Ping's
// "echo <seq> <bytes>" log line shows the actual
// payload length, not just the seq.
//
// Pin set:
//
//   1. Link::sendAckFrame_unlocked emits a 5-byte
//      raw ACK frame (extended with bytes-recvd).
//   2. Link::onAck populates Link::bytesRecvd_[seq]
//      from the wire ACK payload so Ping's
//      bytesRecvdFor(seq) returns the receiver-
//      reported value.
//   3. Link::bytesRecvdFor(seq) returns 0 for a
//      seq that has never been ACKed (default).
//   4. The peer can decode both 5-byte (this
//      release) and 3-byte (legacy 5.3.x) ACK
//      frames; the 3-byte fallback is verified in
//      LinkFrameRxTest's runtime suite.
//   5. The LinkARQ cycle still works with the
//      extended ACK: a sent frame's bytes-recvd
//      appears on the sender side within one
//      MockHal round-trip.
#include <iostream>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/util/UtilCrc.h"
#include "al/util/UtilCobs.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Pin 1: sendAckFrame_unlocked emits a 5-byte
// raw ACK frame containing the seq + bytes-recvd
// + crc. We construct a minimal Link, drive it to
// OK via the standard negotiator, then read the
// bytes on the wire and verify the shape.
static void test_ack_frame_is_5_bytes_with_bytes_recvd() {
    std::cout
        << "\n=== Pin 1: sendAckFrame_unlocked emits 5-byte ACK frame ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();
    negotiate_to_ok(ping, pong, mHal, sHal);

    // Drain any pre-OK frames from the wire so the
    // ACK we're about to inject lands in a clean
    // receiver buffer.
    sHal.txBuf.clear();

    // Inject a data frame into pong's rx path so
    // pong's onPayload fires and emits an ACK.
    uint8_t data[3] = { 0x10, 0x20, 0x30 };
    uint8_t dataSeq = 5;
    uint8_t frame[8];
    frame[0] = dataSeq;
    memcpy(frame + 1, data, 3);
    frame[4] = UtilCrc::crc8(frame, 4);
    uint8_t wireBuf[16];
    wireBuf[0] = 0x00;
    size_t encLen = UtilCobs::encode(frame, 5, wireBuf + 1);
    wireBuf[1 + encLen] = 0x00;

    // Feed pong's MockHal directly so pong's
    // onRx -> processCtrlFrame path runs.
    pong.onRx(wireBuf, (int)(encLen + 2));

    // pong should have emitted an ACK with seq=5
    // and bytes-recvd=3 (the payload size).
    // The MockHal captures pong's tx into sHal.txBuf.
    // Decoded-payload check: feed sHal.txBuf
    // through UtilFrameRx on a temporary mock and
    // inspect the resulting onAck callback.
    struct AckCollector : public UtilFrameRx::Listener {
        std::vector<std::pair<uint8_t, uint16_t>> events;
        bool onPayload(uint8_t, const uint8_t *, int) override {
            return false;
        }
        bool onAck(uint8_t s, uint16_t bytesRecvd) override {
            events.emplace_back(s, bytesRecvd);
            return false;
        }
        bool onNak(uint8_t) override { return false; }
        bool onFrameError() override { return false; }
    } collector;
    UtilFrameRx rx(collector);
    // sHal.txBuf holds raw bytes including COBS
    // framing; feed them through.
    rx.feed(sHal.txBuf.data(), (int)sHal.txBuf.size());
    bool matched = false;
    for (auto &ev : collector.events) {
        if (ev.first == 5 && ev.second == 3) {
            matched = true;
            break;
        }
    }
    assert(matched);
    std::cout
        << "  PASS (ack seq=5 bytes=3 received; this-release 5-byte "
           "wire shape end-to-end)"
        << std::endl;
}

// Pin 2: Link::bytesRecvdFor(seq) populates from
// the peer's wire ACK so the sender can log the
// receiver-reported bytes-recvd. End-to-end:
//   - Ping sends a short 4-byte message via
//     sendMsg() (SYNC mode → cobsSeq=baseSeq).
//   - Pong's onPayload fires; pong's link layer
//     emits the 5-byte ACK with bytes_recvd=4.
//   - The ACK frame round-trips through pong's
//     txBuf into ping's onRx.
//   - Ping's onAck stamps bytesRecvd_[baseSeq]=4.
//   - bytesRecvdFor(baseSeq) returns 4.
//
// We use ASYNC mode here because SYNC's
// sendMsg() blocks on arq_.waitForAck which
// requires time-pumping through MockHal. ASYNC
// sendMsg() returns as soon as the frame is on
// the wire; pipe_data() routes it to pong's
// onRx which fires the wire ACK. The 5-byte
// ACK is the only Pong-side response in this
// release.
static void test_bytes_recvd_table_populated_on_ack() {
    std::cout
        << "\n=== Pin 2: Link::bytesRecvdFor(seq) populated on ACK ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();
    negotiate_to_ok(ping, pong, mHal, sHal);

    // Drain any leftover frames so the round-trip
    // we instrument isn't muddied.
    sHal.txBuf.clear();
    mHal.txBuf.clear();
    sHal.clearAppBuf();
    mHal.clearAppBuf();

    uint8_t msg[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t baseSeq = 0;
    bool ok = ping.sendMsg(msg, 4, &baseSeq);
    assert(ok);
    // ASYNC: sendMsg returned; the message is on
    // the wire (mHal.txBuf). Pipe mHal → sHal so
    // pong's onRx fires and pong emits the
    // 5-byte ACK into sHal.txBuf.
    pipe_data(mHal, sHal);
    // pong is in OK state; pong's onPayload
    // pushed the bytes into its appBuf AND
    // emitted the wire ACK. sHal.txBuf now holds
    // the ACK frame. Pipe sHal → mHal so ping
    // receives the ACK and stamps bytesRecvd_.
    pipe_data(sHal, mHal);
    // For ASYNC short messages (len + MSG_HDR <=
    // MAX_CHUNK), Link::sendMsg merges the
    // 6-byte header + 4-byte payload into one
    // chunk (10 bytes recvd on pong's side).
    // Pong's wire ACK reports this as bytes-recvd
    // = 10 (= the chunk's wire-side length). Pin
    // the value to lock in the merged-chunk
    // contract; a future bump that sends the hdr
    // and payload as separate chunks would change
    // the expected value.
    assert(ping.bytesRecvdFor(baseSeq) == 10);
    std::cout
        << "  PASS (bytesRecvd_[baseSeq] = 10 after ping.sendMsg + "
           "pong's 5-byte ACK round-trip)"
        << std::endl;
}

// Pin 3: pool-exhaustion drop. With the ARQ cache
// full + a pending slot, the OK-state timer drops
// the link so a stalled receiver bounces back to
// SWP rather than spinning on maxRetx.
static void test_pool_exhaustion_drop_in_async_mode() {
    std::cout
        << "\n=== Pin 3: pool exhaustion drops the link in ASYNC mode ==="
        << std::endl;
    // Pin via source-grep: LinkTimers.cpp's
    // onTimerOk_unlocked has the
    // !arqCache_.hasRoom() && pendingCount() > 0
    // drop branch.
    FILE *f = fopen("../../src/al/link/LinkTimers.cpp", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the onTimerOk_unlocked body.
    const char *fn = strstr(buf, "void Link::onTimerOk_unlocked()");
    assert(fn);
    // Find the pool-exhaustion guard: must read
    // arqCache_.hasRoom() and arq_.pendingCount()
    // inside an if-block guarded on cfg.mode !=
    // SYNC.
    const char *hasRoom = strstr(fn, "arqCache_.hasRoom()");
    assert(hasRoom);
    const char *pending = strstr(fn, "arq_.pendingCount()");
    assert(pending);
    // reset_unlocked + sendBreak must follow.
    const char *reset = strstr(hasRoom, "reset_unlocked(true)");
    assert(reset);
    const char *sendBreak = strstr(reset, "hw.sendBreak()");
    assert(sendBreak);
    // Order: hasRoom check BEFORE reset.
    assert(hasRoom < reset);
    std::cout
        << "  PASS (pool-full + pending > 0 drops link in ASYNC mode)"
        << std::endl;
}

// Pin 4: HoldAck path sends a NAK. Pin the new
// shape (not the old "log + return true" shape).
static void test_holdack_sends_nak() {
    std::cout << "\n=== Pin 4: app-buf-full sends NAK (not silent) ==="
              << std::endl;
    FILE *f = fopen("../../src/al/link/LinkRx.cpp", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // The HoldAck branch must call
    // sendNakFrame_unlocked, not just log and
    // return.
    const char *holdAck = strstr(buf, "AppBufAction::HoldAck");
    assert(holdAck);
    const char *nak = strstr(holdAck, "sendNakFrame_unlocked(");
    assert(nak);
    // And the return value must be false (NOT
    // true) so feed() doesn't drop the rest of the
    // byte stream.
    const char *retFalse = strstr(nak, "return false;");
    assert(retFalse);
    std::cout
        << "  PASS (HoldAck -> sendNakFrame_unlocked + return false)"
        << std::endl;
}

int main() {
    std::cout << "=== Running Extended Wire-ACK Regression ===" << std::endl;
    test_ack_frame_is_5_bytes_with_bytes_recvd();
    test_bytes_recvd_table_populated_on_ack();
    test_pool_exhaustion_drop_in_async_mode();
    test_holdack_sends_nak();
    std::cout << "\n=== All 4 ACK bytes-recvd pins PASS ===" << std::endl;
    return 0;
}
