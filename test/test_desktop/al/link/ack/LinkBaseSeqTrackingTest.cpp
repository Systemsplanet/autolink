// Regression test for base-seq tracking at the default 5120-byte
// msg size. The default bump (maxMsg 1024 -> 5120) made the
// multi-chunk path the steady-state, not the corner case. Ping's
// existing matchEcho_ log line sources its byte count from the
// local slot's `len` field, not from the wire-ACK-reported
// bytesRecvd_, so a stray bytesRecvd_==0 for the first chunk of a
// 22-chunk message is not operator-visible through that log line.
// This suite pins the production invariant
// explicitly: the per-message sum equals
// MSG_HDR + payload length, and the per-chunk
// baseSeq_ mapping is correctly populated.
//
// Pins:
//   1. ASYNC 5120-byte (22-chunk) message:
//      bytesRecvdForMessage(baseSeq) returns
//      5126 = MSG_HDR + payload length. Without a
//      per-message sum API, bytesRecvdFor(baseSeq)
//      alone surfaces only 6 (the hdr-only chunk's
//      bytes) — the per-message sum is the right
//      number for any "message N received" log line.
//   2. Short ASYNC message (4-byte payload, 1
//      merged chunk): bytesRecvdFor and
//      bytesRecvdForMessage both return 10
//      (= MSG_HDR + 4). The single-chunk
//      contract is unchanged by the per-message
//      sum addition.
//   3. (Removed) Source-grep on LinkArq.h:
//      baseSeqFor(seq) public accessor existed.
//      The accessor was unused by production code
//      (Link's bytesRecvdForMessage calls
//      arq_.bytesForMessage, which walks the ring
//      without exposing baseSeqFor), so the
//      accessor and its source-grep pin were
//      removed together in the dead-code cleanup.
//   4. Source-grep on Link.h: bytesRecvdForMessage
//      public API exists and delegates to
//      arq_.bytesForMessage. The walk-loop pattern
//      is pinned so a future implementation
//      that reads baseSeq_ through a different
//      path is caught.
//   5. Source-grep on LinkArq.cpp: onSent body stamps
//      baseSeq_[seq] = (baseSeq == NO_BASE) ? seq : baseSeq. Dropping
//      the NO_BASE default would have data chunks with
//      baseSeq_[seq] = 0xFF instead of the hdr-only frame's seq,
//      breaking the per-message sum.
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "MockHal.h"
#include "TestCfg.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"
#include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Drive ping + pong to OK state via the standard
// host negotiator. Drains the wire first so the
// round-trip the test instruments lands in a
// clean receiver buffer.
static void negotiateToOk(Link &ping, Link &pong, MockHal &mHal,
                          MockHal &sHal) {
    lockPair(ping, pong, mHal, sHal);
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
    // The hdr-only frame's bytes-recvd is 6 (= MSG_HDR).
    // bytesRecvdFor(baseSeq) alone returns 6 — useful for "first
    // chunk's length", NOT for the message size.
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

// Pin 3 (removed): no production code reads baseSeq_ through a
// public LinkArq accessor — Link's bytesRecvdForMessage uses
// LinkArq::bytesForMessage internally, which walks the ring without
// exposing a baseSeqFor(seq). Removed along with the unused accessor
// in the dead-code cleanup pass.

// Pin 4: source-grep on Link.h —
// One-owner pin: Link::bytesRecvdForMessage must delegate to the
// ARQ (which owns per-slot metadata); the budget-depth walk over
// baseSeq_ + bytesRecvd_ must live inside LinkArq. A future split
// of that state back across two objects turns this red.
static void test_bytes_recvd_for_message_body() {
    std::cout << "\n=== Pin 4: bytesRecvdForMessage delegates to LinkArq, "
                 "which owns the walk ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/Link.h").c_str(), "r");
    assert(f);
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *m = strstr(buf, "bytesRecvdForMessage(");
    assert(m);
    assert(strstr(m, "arq_.bytesForMessage") &&
           "Link must delegate, not re-own the slot walk");

    FILE *g = fopen(testRepoPath("src/al/link/arq/LinkArq.h").c_str(), "r");
    assert(g);
    char abuf[32768];
    size_t an = fread(abuf, 1, sizeof(abuf) - 1, g);
    abuf[an] = 0;
    fclose(g);
    // Anchor on the actual function SIGNATURE ("uint16_t
    // bytesForMessage(...) const {"), not just any textual
    // mention of the name — LinkArq.h's class comment also
    // mentions "bytesForMessage()" in prose, and a plain
    // strstr(abuf, "bytesForMessage(") would anchor there
    // instead, letting this pin match unrelated text below it
    // (e.g. the class comment's own "(256)" aside) rather than
    // the function body itself.
    const char *am = strstr(abuf, "uint16_t bytesForMessage(");
    assert(am);
    const char *bodyEnd = strchr(am, '}');
    assert(bodyEnd);
    std::string body(am, bodyEnd - am + 1);
    assert(body.find("baseSeq_") != std::string::npos &&
           body.find("bytesRecvd_") != std::string::npos &&
           body.find("ARQ_CHUNK_BUDGET") != std::string::npos &&
           "the budget-depth join lives with the state it joins, "
           "sized from ARQ_CHUNK_BUDGET (not a literal depth)");
    assert(body.find("256") == std::string::npos &&
           "bytesForMessage must NOT walk a literal 256-deep span; "
           "it must be sized from ARQ_CHUNK_BUDGET");
    std::cout << "  PASS" << std::endl;
}

// Pin 5: source-grep on LinkArq.cpp — onSent
// body stamps baseSeq_[budgetIdx(seq)] = (baseSeq ==
// NO_BASE) ? seq : baseSeq. The NO_BASE default is what
// makes the short-message (single merged chunk)
// and hdr-only-frame paths self-baseSeq, while
// the multi-chunk data-frame path can carry an
// explicit baseSeq. Drop the default and the
// 22-chunk message's data chunks would have
// baseSeq_[...] = 0xFF (NO_BASE) and the
// per-message sum would sum 0 chunks (just
// the hdr-only frame's slot, if it was stamped
// with NO_BASE too).
static void test_onSent_stamps_baseseq_with_no_base_default() {
    std::cout << "\n=== Pin 5: LinkArq::onSent stamps baseSeq_[] with the "
                 "NO_BASE default ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/arq/LinkArq.cpp").c_str(), "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the onSent function body.
    const char *fn = strstr(buf, "void LinkArq::onSent(");
    assert(fn);
    // The body must stamp baseSeq_[] through budgetIdx, not a raw
    // [seq] index (a naive idxOf-based lookup drifts once gbnBase_
    // advances), and contain the NO_BASE defaulting.
    const char *stamp = strstr(fn, "baseSeq_[bi]");
    assert(stamp &&
           "onSent must stamp baseSeq_ via a budgetIdx-derived "
           "index, not a raw [seq] index");
    assert(!strstr(fn, "baseSeq_[seq]") &&
           "onSent must NOT index baseSeq_ directly by seq — that index "
           "is gbnBase_-relative-safe only for the idxOf ring, and "
           "baseSeq_ is budgetIdx-backed");
    const char *noBase = strstr(fn, "NO_BASE");
    assert(noBase);
    // The ternary `(baseSeq == NO_BASE) ? seq : baseSeq`
    // must be present (clang-format may break
    // this across lines, so check for the
    // pattern loosely).
    const char *ternary = strstr(fn, "baseSeq == NO_BASE");
    assert(ternary);
    std::cout << "  PASS (LinkArq::onSent stamps baseSeq_[budgetIdx(seq)] with "
                 "NO_BASE default)"
              << std::endl;
}

// Pin 6: two ASYNC messages sent back-to-back in the same session.
// onAcked() must NOT zero baseSeq_[seq] on ACK — a second message's
// baseSeq is nonzero, and zeroing each chunk's baseSeq_ entry as it
// is ACKed makes bytesForMessage(secondBaseSeq) sum 0 chunks.
// Toggle onAcked() to clear baseSeq_[bi] on ACK -> red.
static void test_two_messages_baseseq_sum_independently() {
    std::cout << "\n=== Pin 6: second message's bytesForMessage is correct "
                 "(onAcked no longer clears baseSeq_) ==="
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

    uint8_t msg1[4] = { 1, 2, 3, 4 };
    uint8_t baseSeq1 = 0;
    assert(ping.sendMsg(msg1, 4, &baseSeq1));
    pumpRounds(mHal, sHal, 20);

    uint8_t msg2[8] = { 5, 6, 7, 8, 9, 10, 11, 12 };
    uint8_t baseSeq2 = 0;
    assert(ping.sendMsg(msg2, 8, &baseSeq2));
    pumpRounds(mHal, sHal, 20);

    assert(baseSeq2 != baseSeq1 &&
           "the two messages must land on different baseSeqs for this "
           "pin to actually exercise the fix");

    LinkTestAccessor t(ping);
    // Both messages are single merged chunks (MSG_HDR + len <=
    // MAX_CHUNK), so each's per-message sum is MSG_HDR + its own
    // payload length — and querying the SECOND message's baseSeq
    // must not pick up the first message's already-ACKed bytes.
    uint16_t sum1 = t.bytesRecvdForMessage(baseSeq1);
    uint16_t sum2 = t.bytesRecvdForMessage(baseSeq2);
    if (sum1 != 10 || sum2 != 14) {
        std::cerr << "\nFAIL: bytesRecvdForMessage(baseSeq1=" << (int)baseSeq1
                  << ")=" << sum1 << " (want 10), "
                  << "bytesRecvdForMessage(baseSeq2=" << (int)baseSeq2
                  << ")=" << sum2 << " (want 14)" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (message 1 sum=" << sum1 << ", message 2 sum=" << sum2
              << ", independently correct)" << std::endl;
}

// Pin 7: three full-window (32-chunk) messages sent back-to-back
// push seq all the way through one full budgetIdx (=seq %
// ARQ_CHUNK_BUDGET, ARQ_CHUNK_BUDGET = 64) wrap. Message 1 occupies
// budgetIdx 0..31, message 2 occupies 32..63 (no overlap with
// message 1), and message 3 — seq 64..95 — wraps back onto
// budgetIdx 0..31, REUSING message 1's physical slots. This is
// the worst case the class comment's sizing claim rests on: "a
// slot's previous occupant is always ACKed before a new send can
// reuse it" is asserted, not verified, until a test actually
// drives seq past one full budget cycle and checks the reused
// slots hold the NEW message's data, not stale residue from the
// old occupant. Toggle ARQ_CHUNK_BUDGET down to
// AUTOLINK_ARQ_PIPELINE_WINDOW (32, same depth as the window) ->
// message 3 collides with message 2 (still-live) instead of
// message 1 (long-acked) -> red.
static void test_three_full_window_messages_wrap_budget_ring() {
    std::cout << "\n=== Pin 7: three 32-chunk messages wrap one full "
                 "budgetIdx cycle cleanly ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.streamBufferSize = 262144;
    // 7750-byte payload -> chunksForMsgLen == 1 (hdr) + 31 (data)
    // == 32 == AUTOLINK_ARQ_PIPELINE_WINDOW, exactly filling the
    // GBN window with a single message (the tightest admission
    // case: inflight(0) + chunks(32) == window(32)).
    cfg.maxMsg = 7750;
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

    const int msgLen = 7750;
    const uint16_t wantSum = (uint16_t)(MSG_HDR + msgLen); // 7756

    std::vector<uint8_t> tx(msgLen);
    uint8_t baseSeqs[3];
    for (int m = 0; m < 3; m++) {
        for (int i = 0; i < msgLen; i++)
            tx[i] = (uint8_t)((m * 37 + i) & 0xFF);
        // RateLimited is retryable backpressure, not a
        // rejection: three 7750-byte messages back to back
        // is 23250 bytes, well over one 1 s budget at the
        // locked baud. A real app pumps and retries; so do
        // we, bounded, so a genuine refusal still trips.
        bool ok = false;
        for (int attempt = 0; attempt < 600 && !ok; attempt++) {
            ok = ping.sendMsg(tx.data(), msgLen, &baseSeqs[m]);
            if (ok)
                break;
            if (LinkTestAccessor(ping).lastSendMsgReasonForTest() !=
                SendMsgReason::RateLimited)
                break;
            mHal.pumpClock(5);
            sHal.pumpClock(5);
            pipe_data(mHal, sHal);
            pipe_data(sHal, mHal);
        }
        if (!ok) {
            std::cerr << "\nFAIL: sendMsg rejected message " << m
                      << " (baseSeq so far:";
            for (int k = 0; k < m; k++)
                std::cerr << " " << (int)baseSeqs[k];
            std::cerr << ")" << std::endl;
            assert(false);
        }
        // 32 chunks landed in mHal.txBuf; pump the wire ACKs back
        // so every chunk's bytesRecvd_ is populated before the
        // NEXT message reuses any budgetIdx slots.
        pumpRounds(mHal, sHal, 150);
    }

    LinkTestAccessor t(ping);
    uint16_t sum0 = t.bytesRecvdForMessage(baseSeqs[0]);
    uint16_t sum1 = t.bytesRecvdForMessage(baseSeqs[1]);
    uint16_t sum2 = t.bytesRecvdForMessage(baseSeqs[2]);

    std::cout << "  baseSeqs=" << (int)baseSeqs[0] << "," << (int)baseSeqs[1]
              << "," << (int)baseSeqs[2] << " (budgetIdx "
              << (int)(baseSeqs[0] % ARQ_CHUNK_BUDGET) << ","
              << (int)(baseSeqs[1] % ARQ_CHUNK_BUDGET) << ","
              << (int)(baseSeqs[2] % ARQ_CHUNK_BUDGET) << ")" << std::endl;

    // Message 3's budgetIdx must equal message 1's — this pin is
    // meaningless if the two messages didn't actually land on the
    // same physical slots.
    if (baseSeqs[2] % ARQ_CHUNK_BUDGET != baseSeqs[0] % ARQ_CHUNK_BUDGET) {
        std::cerr << "\nFAIL: message 3 (budgetIdx="
                  << (int)(baseSeqs[2] % ARQ_CHUNK_BUDGET)
                  << ") did not land on message 1's slot (budgetIdx="
                  << (int)(baseSeqs[0] % ARQ_CHUNK_BUDGET)
                  << ") — this pin needs a genuine wrap to mean anything; "
                  << "check ARQ_CHUNK_BUDGET / AUTOLINK_ARQ_PIPELINE_WINDOW"
                  << std::endl;
        assert(false);
    }
    // Message 3's sum must be correct — proving the reused slots
    // hold ITS data, not message 1's stale residue.
    if (sum2 != wantSum) {
        std::cerr << "\nFAIL: message 3 bytesRecvdForMessage=" << sum2
                  << " (want " << wantSum
                  << ") — budgetIdx wrap corrupted the reused slots"
                  << std::endl;
        assert(false);
    }
    // Message 2 (whose slots were never reused) must still be
    // correct too — a wrap bug touching neighboring slots would
    // show up here even if message 3 happened to read back right.
    if (sum1 != wantSum) {
        std::cerr << "\nFAIL: message 2 bytesRecvdForMessage=" << sum1
                  << " (want " << wantSum << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (message 1 sum=" << sum0 << ", message 2 sum=" << sum1
              << ", message 3 sum=" << sum2
              << " — budgetIdx wrap reused message 1's slots cleanly for "
                 "message 3)"
              << std::endl;
}

int main() {
    std::cout << "=== Running LinkBaseSeqTrackingTest Tests ===" << std::endl;
    Log::log().setLevel(Log::WARNING);

    test_async_22chunk_baseseq_message_sum();
    test_async_short_baseseq_message_sum();
    test_bytes_recvd_for_message_body();
    test_onSent_stamps_baseseq_with_no_base_default();
    test_two_messages_baseseq_sum_independently();
    test_three_full_window_messages_wrap_budget_ring();

    std::cout
        << "\n=== LinkBaseSeqTrackingTest Tests Completed Successfully ==="
        << std::endl;
    return 0;
}
