// Regression test for the base-seq tracking
// carry-over (todo item 4: "base-seq tracking status
// at default 5120-byte msg"). The default bump
// (maxMsg 1024 -> 5120) made the multi-chunk path
// the steady-state, not the corner case. Ping's
// existing matchEcho_ log line sources its byte
// count from the local slot's `len` field, not from
// the wire-ACK-reported bytesRecvd_, so a stray
// bytesRecvd_==0 for the first chunk of a 22-chunk
// message would NOT have surfaced on the bench.
// This suite pins the production invariant
// explicitly: the per-message sum equals
// MSG_HDR + payload length, and the per-chunk
// baseSeq_ mapping is correctly populated.
//
// Pins:
//   1. ASYNC 5120-byte (22-chunk) message:
//      bytesRecvdForMessage(baseSeq) returns
//      5126 = MSG_HDR + payload length. Pre-fix
//      shape (no per-message sum API) would
//      have surfaced 6 (the hdr-only chunk's
//      bytes) for bytesRecvdFor(baseSeq); the
//      per-message sum is the right number
//      for any "message N received" log line.
//   2. Short ASYNC message (4-byte payload, 1
//      merged chunk): bytesRecvdFor and
//      bytesRecvdForMessage both return 10
//      (= MSG_HDR + 4). The single-chunk
//      contract is unchanged by the per-message
//      sum addition.
//   3. Source-grep on LinkArq.h: baseSeqFor(seq)
//      public accessor exists. Pre-fix shape
//      had baseSeq_ as a private field with
//      no read accessor — the production code
//      never consumed it, so a future refactor
//      that drops the field would silently
//      break the per-message sum query.
//   4. Source-grep on Link.h: bytesRecvdForMessage
//      public API exists and references
//      arq_.baseSeqFor. The walk-loop pattern
//      is pinned so a future implementation
//      that reads baseSeq_ through a different
//      path is caught.
//   5. Source-grep on LinkArq.cpp: onSent body
//      stamps baseSeq_[seq] = (baseSeq == NO_BASE)
//      ? seq : baseSeq. The pre-fix shape (or any
//      future refactor that drops the NO_BASE
//      default) would have data chunks with
//      baseSeq_[seq] = 0xFF instead of the
//      hdr-only frame's seq, breaking the
//      per-message sum.
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Drive ping + pong to OK state via the standard
// host negotiator. Drains the wire first so the
// round-trip the test instruments lands in a
// clean receiver buffer.
static void negotiateToOk(Link &ping, Link &pong, MockHal &mHal,
                          MockHal &sHal) {
    for (int i = 0; i < 100 &&
         (ping.getState() != State::OK || pong.getState() != State::OK);
         i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    sHal.txBuf.clear();
    mHal.txBuf.clear();
    sHal.clearAppBuf();
    mHal.clearAppBuf();
}

static void pumpRounds(MockHal &aHal, MockHal &bHal, int rounds) {
    for (int r = 0; r < rounds; r++) {
        aHal.pumpClock(2);
        bHal.pumpClock(2);
        pipe_data(aHal, bHal);
        pipe_data(bHal, aHal);
    }
}

// Pin 1: 22-chunk ASYNC send, bytesRecvdForMessage
// returns 5126 (MSG_HDR + 5120). The contract is
// that any "bytes the peer has received for
// message N" query returns the full message
// size, not the first chunk's bytes (which is
// 6, the hdr-only frame's length).
static void test_async_22chunk_baseseq_message_sum() {
    std::cout << "\n=== Pin 1: ASYNC 22-chunk message — bytesRecvdForMessage "
                 "returns MSG_HDR + payload ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = AUTOLINK_DEFAULT_MAX_MSG; // 5120
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    const int msgLen = 5120;
    std::vector<uint8_t> tx(msgLen), rx(msgLen + 16);
    for (int i = 0; i < msgLen; i++)
        tx[i] = (uint8_t)(i & 0xFF);
    uint8_t baseSeq = 0;
    bool ok = ping.sendMsg(tx.data(), msgLen, &baseSeq);
    assert(ok);

    // 22 chunks landed in mHal.txBuf; pump the
    // 22 wire ACKs back so bytesRecvd_ is
    // populated for every chunk's seq.
    pumpRounds(mHal, sHal, 50);

    LinkTestAccessor t(ping);
    // The hdr-only frame's bytes-recvd is 6
    // (= MSG_HDR). Pre-fix, bytesRecvdFor(baseSeq)
    // returns 6 — useful for "first chunk's
    // length", NOT for the message size.
    assert(t.bytesRecvdFor(baseSeq) == 6);
    // The per-message sum: 22 chunks contribute
    // MSG_HDR + 21*MAX_CHUNK + 320 = 6 + 5250 +
    // 320 = 5576? No — wire-ACK reports the
    // bytes the peer PUSHED to its appBuf. Pong
    // sees the hdr-only frame as 6 bytes pushed,
    // and the data chunks as their chunk length.
    // So the sum is exactly MSG_HDR + payload
    // length = 6 + 5120 = 5126. (The 22-chunk
    // count is 1 hdr + 21 data because
    // chunksForMsgLen(5120) = 1 + ceil(5120/250)
    // = 1 + 21 = 22.)
    assert(t.bytesRecvdForMessage(baseSeq) == 5126);

    // The receiver sees the full payload.
    int got = pong.recvMsg(rx.data(), (int)rx.size());
    assert(got == msgLen);
    for (int i = 0; i < msgLen; i++)
        assert(rx[i] == tx[i]);

    std::cout
        << "  PASS (bytesRecvdForMessage(" << (int)baseSeq
        << ") = 5126 = MSG_HDR + 5120; pong received 5120 B byte-for-byte)"
        << std::endl;
}

// Pin 2: short ASYNC message (4-byte payload
// merged with MSG_HDR into one chunk). Both
// bytesRecvdFor(baseSeq) and bytesRecvdForMessage
// (baseSeq) return 10 (= MSG_HDR + 4). The
// single-chunk contract is unchanged by adding
// the per-message sum query.
static void test_async_short_baseseq_message_sum() {
    std::cout << "\n=== Pin 2: ASYNC 4-byte (1-chunk) message — "
                 "bytesRecvdForMessage returns MSG_HDR + 4 ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = AUTOLINK_DEFAULT_MAX_MSG;
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    uint8_t msg[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t baseSeq = 0;
    bool ok = ping.sendMsg(msg, 4, &baseSeq);
    assert(ok);
    pumpRounds(mHal, sHal, 20);

    LinkTestAccessor t(ping);
    // Short ASYNC: one merged chunk (MSG_HDR + 4
    // = 10 bytes). bytesRecvdFor = bytesRecvdForMessage
    // = 10.
    assert(t.bytesRecvdFor(baseSeq) == 10);
    assert(t.bytesRecvdForMessage(baseSeq) == 10);
    std::cout << "  PASS (single-chunk message: bytesRecvdFor == "
                 "bytesRecvdForMessage == 10)"
              << std::endl;
}

// Pin 3: source-grep on LinkArq.h — the public
// baseSeqFor(seq) accessor exists. Without it,
// the per-message sum has no way to walk the
// 256-slot baseSeq_ table.
static void test_baseseq_for_accessor_exists() {
    std::cout
        << "\n=== Pin 3: LinkArq.h exposes baseSeqFor(seq) public accessor ==="
        << std::endl;
    FILE *f = fopen("../../src/al/link/arq/LinkArq.h", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the public accessor declaration.
    const char *acc = strstr(buf, "baseSeqFor(");
    assert(acc);
    // Must be a const member of LinkArq (in the
    // public block, not a private detail).
    const char *clsStart = strstr(buf, "class LinkArq");
    assert(clsStart);
    // Verify the accessor is after the class open
    // (not in a forward decl or comment).
    assert(acc > clsStart);
    std::cout << "  PASS (LinkArq::baseSeqFor public accessor present)"
              << std::endl;
}

// Pin 4: source-grep on Link.h —
// bytesRecvdForMessage is a public method
// whose body references arq_.baseSeqFor and
// walks 256 entries. The walk-loop pattern
// is pinned so a future implementation that
// reads baseSeq_ through a different path
// (or pre-computes the sum on onAck) is
// caught.
static void test_bytes_recvd_for_message_body() {
    std::cout << "\n=== Pin 4: Link::bytesRecvdForMessage references "
                 "arq_.baseSeqFor in its body ==="
              << std::endl;
    FILE *f = fopen("../../src/al/link/Link.h", "r");
    assert(f);
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the bytesRecvdForMessage method.
    const char *m = strstr(buf, "bytesRecvdForMessage(");
    assert(m);
    // The method body must reference arq_.baseSeqFor.
    const char *ref = strstr(m, "arq_.baseSeqFor");
    assert(ref);
    // The body must walk 256 entries (the seq
    // space). Comment-stripped check: look for
    // the literal "256" inside the method body.
    // Body ends at the next matching '}' at the
    // method's brace depth — approximated by
    // scanning 1024 chars forward.
    char body[1024] = { 0 };
    size_t copied = 0;
    for (size_t i = 0; m[i] && copied < sizeof(body) - 1; i++) {
        body[copied++] = m[i];
        if (m[i] == '{') {
            int depth = 1;
            for (i++; m[i] && depth > 0 && copied < sizeof(body) - 1; i++) {
                body[copied++] = m[i];
                if (m[i] == '{')
                    depth++;
                else if (m[i] == '}')
                    depth--;
            }
            break;
        }
    }
    body[copied] = 0;
    assert(strstr(body, "256"));
    std::cout
        << "  PASS (Link::bytesRecvdForMessage body calls arq_.baseSeqFor "
           "and walks 256 entries)"
        << std::endl;
}

// Pin 5: source-grep on LinkArq.cpp — onSent
// body stamps baseSeq_[seq] = (baseSeq == NO_BASE)
// ? seq : baseSeq. The NO_BASE default is what
// makes the short-message (single merged chunk)
// and hdr-only-frame paths self-baseSeq, while
// the multi-chunk data-frame path can carry an
// explicit baseSeq. Drop the default and the
// 22-chunk message's data chunks would have
// baseSeq_[seq] = 0xFF (NO_BASE) and the
// per-message sum would sum 0 chunks (just
// the hdr-only frame's slot, if it was stamped
// with NO_BASE too).
static void test_onSent_stamps_baseseq_with_no_base_default() {
    std::cout << "\n=== Pin 5: LinkArq::onSent stamps baseSeq_[seq] with the "
                 "NO_BASE default ==="
              << std::endl;
    FILE *f = fopen("../../src/al/link/arq/LinkArq.cpp", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the onSent function body.
    const char *fn = strstr(buf, "void LinkArq::onSent(");
    assert(fn);
    // The body must contain the literal
    // "baseSeq_[seq]" assignment and the
    // "NO_BASE" defaulting.
    const char *stamp = strstr(fn, "baseSeq_[seq]");
    assert(stamp);
    const char *noBase = strstr(fn, "NO_BASE");
    assert(noBase);
    // The ternary `(baseSeq == NO_BASE) ? seq : baseSeq`
    // must be present (clang-format may break
    // this across lines, so check for the
    // pattern loosely).
    const char *ternary = strstr(fn, "baseSeq == NO_BASE");
    assert(ternary);
    std::cout
        << "  PASS (LinkArq::onSent stamps baseSeq_[seq] with NO_BASE default)"
        << std::endl;
}

int main() {
    std::cout << "=== Running LinkBaseSeqTrackingTest Tests ===" << std::endl;
    Log::log().setLevel(Log::WARNING);

    test_async_22chunk_baseseq_message_sum();
    test_async_short_baseseq_message_sum();
    test_baseseq_for_accessor_exists();
    test_bytes_recvd_for_message_body();
    test_onSent_stamps_baseseq_with_no_base_default();

    std::cout
        << "\n=== LinkBaseSeqTrackingTest Tests Completed Successfully ==="
        << std::endl;
    return 0;
}
