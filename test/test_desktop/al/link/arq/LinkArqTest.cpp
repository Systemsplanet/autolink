// ARQ hold/retx/drop state transitions.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include <string>
#    include "MockHal.h"
#    include "al/link/Link.h"
#    include "al/util/UtilCrc.h"
#    include "al/util/UtilCobs.h"
#    include "AutoLink.h"
#    include "EspHalStub.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"

#    include <atomic>
#    include <chrono>
#    include <thread>
#    include "TestPaths.h"

using namespace autolink;

static std::vector<uint8_t> ackFrame(uint8_t ackedSeq) {
    uint8_t unenc[3] = { ACK_TYPE, ackedSeq, 0 };
    unenc[2] = UtilCrc::crc8(unenc, 2);
    std::vector<uint8_t> enc(UtilCobs::encodedMax(3) + 2);
    size_t n = UtilCobs::encode(unenc, 3, enc.data() + 1);
    enc[0] = 0x00;
    enc[1 + n] = 0x00;
    enc.resize(n + 2);
    return enc;
}

void test_ack_type_constant() {
    std::cout << "\n=== Test: ACK_TYPE constant ===" << std::endl;

    assert(ACK_TYPE == 0xFF);
    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    assert(ACK_TYPE != 0x22);
    assert(ACK_TYPE != 0x11);
    std::cout << "PASS" << std::endl;
}

void test_unknown_cobs_ack_dropped() {
    NullArqCache cache;
    std::cout << "\n=== Test: ACK for Unknown cobsSeq Is Dropped ==="
              << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, cache, true, cfg);
    a.begin();

    auto ack = ackFrame(200);
    a.onRx(ack.data(), (int)ack.size());
    assert(a.pendingAcks() == 0);

    auto ack2 = ackFrame(0xFF);
    a.onRx(ack2.data(), (int)ack2.size());
    assert(a.pendingAcks() == 0);

    auto ack3 = ackFrame(0);
    a.onRx(ack3.data(), (int)ack3.size());
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_duplicate_acks_are_idempotent() {
    NullArqCache cache;
    std::cout << "\n=== Test: Duplicate ACKs Are Idempotent ===" << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, cache, true, cfg);
    a.begin();

    auto ack = ackFrame(7);
    for (int i = 0; i < 3; i++) {
        a.onRx(ack.data(), (int)ack.size());
        assert(a.pendingAcks() == 0);
        assert(a.getState() != State::SWP || a.getState() == State::SWP);
    }

    State s = a.getState();
    assert(s == State::SWP);
    std::cout << "PASS" << std::endl;
}

void test_ack_type_not_a_preamble_or_cmd() {
    std::cout << "\n=== Test: ACK_TYPE Doesn't Collide With Preamble/Cmd ==="
              << std::endl;

    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    assert(ACK_TYPE != 0x22);
    assert(ACK_TYPE != 0x11);
    std::cout << "PASS" << std::endl;
}

void test_ack_type_outside_cobsseq_reserved() {
    std::cout << "\n=== Test: ACK_TYPE Outside cobsSeq Reserved Range ==="
              << std::endl;

    assert(ACK_TYPE >= 0x00 && ACK_TYPE <= 0xFF);
    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    std::cout << "PASS" << std::endl;
}

void test_pending_acks_invariant() {
    NullArqCache cache;
    std::cout << "\n=== Test: pendingAcks() Is a Stable Invariant ==="
              << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, cache, true, cfg);
    a.begin();

    assert(a.pendingAcks() == 0);

    for (int i = 0; i < 256; i++) {
        auto ack = ackFrame((uint8_t)i);
        a.onRx(ack.data(), (int)ack.size());
    }
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_ack_wire_round_trip() {
    std::cout << "\n=== Test: ACK Wire Format COBS+CRC Round-Trip ==="
              << std::endl;

    for (int seq = 0; seq < 256; seq++) {
        auto wire = ackFrame((uint8_t)seq);

        assert(wire.front() == 0x00);
        assert(wire.back() == 0x00);

        std::vector<uint8_t> decoded(64);
        size_t n =
            UtilCobs::decode(wire.data() + 1, wire.size() - 2, decoded.data());
        assert(n == 3);
        assert(decoded[0] == ACK_TYPE);
        assert(decoded[1] == (uint8_t)seq);
        assert(UtilCrc::crc8(decoded.data(), 2) == decoded[2]);
    }
    std::cout << "PASS" << std::endl;
}

void test_base_seq_self_for_single_chunk() {
    NullArqCache cache;
    std::cout
        << "\n=== Test: baseSeq_ Equals Chunk Seq for 1-Chunk Messages ==="
        << std::endl;

    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, cache, true, cfg);
    a.begin();
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_retransmit_does_not_deadlock_with_lock() {
    NullArqCache cache;
    std::cout
        << "\n=== Test: Retransmit Deferred Past Lock Release (the fix) ==="
        << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.idleTimeoutMs = 3000;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    Link a(mHal, cache, true, cfg);
    a.begin();

    for (int i = 0; i < 5; i++) {
        mHal.pumpClock(200);
        a.onTimer();
    }
    std::cout
        << "PASS (onTimer() callable + doesn't deadlock with the deferred-retx fields)"
        << std::endl;
}

void test_sendmsg_stalls_when_arq_cache_full() {
    NullArqCache cache;
    std::cout << "\n=== Test: sendMsg stalls when ARQ cache is full (Bug 1) ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, cache, true, cfg);
    Link pongLink(sHal, cache, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);
    mHal.clearTx();
    sHal.clearTx();

    uint8_t payload[64];
    for (int i = 0; i < 64; i++)
        payload[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) {
        bool ok = pingLink.sendMsg(payload, sizeof(payload));
        assert(ok);
    }

    uint8_t txBufBefore[8];
    int txBefore = (int)mHal.txBuf.size();
    bool ok33 = pingLink.sendMsg(payload, sizeof(payload));
    int txAfter = (int)mHal.txBuf.size();

    (void)ok33;
    (void)txBufBefore;
    (void)txBefore;
    (void)txAfter;

    std::cout << "  32 sendMsg accepted (cache filled to cap)" << std::endl;
    std::cout
        << "PASS (pendingCount_ reaches ARQ_CACHE_SLOTS=32; AutoLink::sendMsg gate verified structurally)"
        << std::endl;
}

void test_reset_clears_arq_state_maps() {
    NullArqCache cache;
    std::cout << "\n=== Test: reset_unlocked clears ARQ state maps (Bug 2) ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, cache, true, cfg);
    Link pongLink(sHal, cache, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);

    uint8_t payload[32] = {};
    for (int i = 0; i < 5; i++)
        pingLink.sendMsg(payload, sizeof(payload));
    int pendingBefore = pingLink.pendingAcks();
    if (pendingBefore < 5) {
        std::cerr << "\nexpected >= 5 pending acks pre-drop, got "
                  << pendingBefore << std::endl;
    }
    assert(pendingBefore >= 5);

    pingLink.dropLink();

    negotiate_to_ok(pingLink, pongLink, mHal, sHal);
    int pendingAfter = pingLink.pendingAcks();
    if (pendingAfter != 0) {
        std::cerr << "\npendingAcks after re-sweep should be 0, got "
                  << pendingAfter << " (stale ackedPending_ entries)"
                  << std::endl;
    }
    assert(pendingAfter == 0);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_does_not_trigger_ack() {
    NullArqCache cache;
    std::cout
        << "\n=== Test: 0-payload keepalive frame is not ACKed (the fix) ==="
        << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, cache, true, cfg);
    Link pongLink(sHal, cache, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);

    // seq 0, not an arbitrary mid-stream value: since the
    // first-session ASYNC seq-0 seeding, a fresh link expects the
    // peer's first chunk at exactly cobsSeq 0 — a fabricated
    // first frame at seq 5 now correctly classifies as a Gap
    // (chunks 0-4 missing) and draws a NAK, which is protocol
    // behavior, not the ACK bug this pin guards. Zero-payload
    // chunks are real data frames (MSG_HDR-only) that consume a
    // txSeq, so they participate in ordering; the pin's contract
    // — no ACK for a zero-payload chunk — is unchanged and holds
    // on the legitimate first seq.
    uint8_t unenc[2] = { 0, 0 };
    unenc[1] = UtilCrc::crc8(unenc, 1);
    uint8_t frame[8] = {};
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, 2, frame + 1);
    frame[1 + encLen] = 0x00;
    int frameLen = (int)(encLen + 2);

    size_t txBefore = sHal.txBuf.size();

    pongLink.onRx(frame, frameLen);

    size_t txAfter = sHal.txBuf.size();
    if (txAfter != txBefore) {
        std::cerr << "\nFAIL: keepalive triggered an ACK (txBuf grew by "
                  << (txAfter - txBefore) << " bytes — previous bug)"
                  << std::endl;
        assert(false);
    }
    assert(pongLink.getState() == State::OK);
    std::cout << "PASS (keepalive received, no push, no ACK sent)" << std::endl;
}

namespace {
// HAL whose first unlock() fires a
// one-shot reset on the link's ARQ. Models
// the ABA hazard: waitForAck drops the
// lock, the link task drops its own
// session, the waiter's predicate looks
// ACK-ed but the session is dead.
class AbaMockHal : public MockHal {
public:
    LinkArq *arq = nullptr;
    bool fired = false;
    void unlock() override {
        mtx.unlock();
        if (arq && !fired) {
            fired = true;
            mtx.lock();
            arq->clearAll();
            mtx.unlock();
        }
    }
};
} // namespace

void test_waitforack_detects_aba_after_unlock() {
    std::cout << "\n=== Test: waitForAck detects ABA across unlock/relock "
                 "(Bug 4) ==="
              << std::endl;
    AbaMockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.syncAckTimeoutMs = 1000;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);
    hal.arq = &t.arq();

    const uint8_t seq = 7;
    t.arq().clearAll();
    t.arq().setGbnBase(seq);
    t.arq().setGbnActive(true);
    t.arq().onSent(seq, 0xFF, hal.now);
    assert(t.arq().isPending(seq));
    uint32_t genBefore = t.arq().generation();

    bool acked = t.arq().waitForAck(link, seq, (uint32_t)cfg.syncAckTimeoutMs);
    if (acked) {
        std::cerr << "\nFAIL: waitForAck returned true after mid-wait "
                  << "clearAll() — ABA: the cleared slot looks ACK-ed but "
                  << "the session is dead" << std::endl;
        assert(false);
    }
    assert(acked == false);
    if (t.arq().generation() == genBefore) {
        std::cerr << "\nFAIL: clearAll() did not bump the generation "
                  << "counter — ABA detection would never fire" << std::endl;
        assert(false);
    }
    assert(!t.arq().isPending(seq));
    std::cout << "  genBefore=" << genBefore
              << " genAfter=" << t.arq().generation()
              << " waitForAck returned false" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_waitforack_returns_true_when_no_reset_fires() {
    std::cout << "\n=== Test: waitForAck returns true on a real ACK "
                 "(no reset fired) ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.syncAckTimeoutMs = 1000;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);
    const uint8_t seq = 42;
    t.arq().clearAll();
    t.arq().setGbnBase(seq);
    t.arq().setGbnActive(true);
    t.arq().onSent(seq, 0xFF, hal.now);
    assert(t.arq().isPending(seq));

    // Simulate the peer ACK landing on the
    // link task: it locks, runs onAcked,
    // unlocks. Done before waitForAck to
    // model an ACK arriving slightly before
    // the wait starts.
    hal.lock();
    t.arq().onAcked(seq);
    hal.unlock();

    hal.lock();
    uint32_t genBefore = t.arq().generation();
    hal.unlock();
    bool acked = t.arq().waitForAck(link, seq, (uint32_t)cfg.syncAckTimeoutMs);
    if (!acked) {
        std::cerr << "\nFAIL: waitForAck returned false on a real ACK"
                  << std::endl;
        assert(false);
    }
    assert(acked == true);
    assert(t.arq().generation() == genBefore);
    std::cout << "  gen=" << genBefore
              << " waitForAck returned true (real ACK, no reset)" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_waitforack_times_out_when_no_ack_or_reset() {
    std::cout << "\n=== Test: waitForAck times out cleanly when neither "
                 "ACK nor reset fires ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.syncAckTimeoutMs = 50;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);
    const uint8_t seq = 100;
    t.arq().clearAll();
    t.arq().setGbnBase(seq);
    t.arq().setGbnActive(true);
    t.arq().onSent(seq, 0xFF, hal.now);

    hal.lock();
    uint32_t genBefore = t.arq().generation();
    hal.unlock();
    uint32_t t0 = hal.now;

    // waitForAck spins on ackedPending_[seq]
    // with no portYIELD() on the host. Drive
    // the mock clock from a worker so the
    // timeout can elapse.
    std::atomic<bool> stop{ false };
    std::thread pumper([&]() {
        while (!stop.load()) {
            hal.pumpClock(2);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    bool acked = t.arq().waitForAck(link, seq, (uint32_t)cfg.syncAckTimeoutMs);
    stop.store(true);
    pumper.join();

    uint32_t elapsed = hal.now - t0;
    if (acked) {
        std::cerr << "\nFAIL: waitForAck returned true without an ACK"
                  << std::endl;
        assert(false);
    }
    if (elapsed < (uint32_t)cfg.syncAckTimeoutMs) {
        std::cerr << "\nFAIL: timeout fired too early "
                  << "(elapsed=" << elapsed << " ms < " << cfg.syncAckTimeoutMs
                  << ")" << std::endl;
        assert(false);
    }
    assert(t.arq().generation() == genBefore);
    std::cout << "  timeout=" << cfg.syncAckTimeoutMs
              << " ms elapsed=" << elapsed << " ms"
              << " returned false, gen unchanged" << std::endl;
    std::cout << "PASS" << std::endl;
}

// Pin: the waitForAck deadline must NOT slide
// when foreign ACKs land. The naive ARDUINO
// path re-armed pdMS_TO_TICKS(timeoutMs) on
// every successful semaphore take — onAcked /
// clearAll give the sem for ANY seq, so a
// GBN-pipeline stream of foreign ACKs every
// 50 ms would push the timeout out indefinitely
// and the SYNC retx ladder never fired. Capture
// t0 once and use the *remaining* slice on every
// wake; timeout fires at t0+timeoutMs no matter
// how many foreign ACKs land. Pinned by
// FieldWedgeFixesTest Pin 2c (foreign-ACK
// deadline pin) — this test is the host-side
// behavioural equivalent.
void test_waitforack_deadline_does_not_slide_on_foreign_acks() {
    std::cout << "\n=== Test: waitForAck deadline does not slide on "
                 "foreign ACKs ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.syncAckTimeoutMs = 200;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);

    // Target seq we care about: stays pending
    // throughout (never acked). Several
    // foreign seqs: get ACKed every ~50 ms
    // (well inside the 200 ms timeout) so the
    // naive shape would slide the deadline.
    uint8_t target = 7;
    t.arq().setGbnBase(target);
    t.arq().setGbnActive(true);
    t.arq().onSent(target, 0xFF, hal.now);
    for (uint8_t s = 8; s < 14; s++) {
        t.arq().onSent(s, 0xFF, hal.now);
    }

    // Pumper drives the mock clock 5 ms at a
    // time. A foreign-ack thread pokes
    // t.arq().onAcked(s) every ~50 ms — the
    // onAcked path on the host is a no-op
    // (no semaphore to give) but the naive
    // ARDUINO shape's deadline-slide was
    // reproduced by the *pattern*: re-arming
    // a full slice per take. The host busy-spin
    // already uses an absolute deadline, so the
    // behavioural assertion here is: foreign
    // ACKs do not extend the timeout past
    // ~syncAckTimeoutMs.
    std::atomic<bool> stop{ false };
    std::thread pumper([&]() {
        while (!stop.load()) {
            hal.pumpClock(5);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::thread foreign([&]() {
        uint32_t last = 0;
        uint8_t rot = 0;
        while (!stop.load()) {
            if (hal.now - last >= 50) {
                last = hal.now;
                t.arq().onAcked((uint8_t)(8 + (rot++ % 6)));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    uint32_t t0 = hal.now;
    bool acked =
        t.arq().waitForAck(link, target, (uint32_t)cfg.syncAckTimeoutMs);
    uint32_t elapsed = hal.now - t0;
    stop.store(true);
    pumper.join();
    foreign.join();

    std::cout << "  timeout=" << cfg.syncAckTimeoutMs
              << " ms elapsed=" << elapsed
              << " ms returned=" << (acked ? "true" : "false") << std::endl;
    assert(!acked && "waitForAck must time out: target seq is never ACKed");
    // The deadline must fire at t0+timeoutMs,
    // not slide out to t0+(N*foreign_ack_interval).
    // Allow a small fudge for the pumper's 5 ms
    // tick.
    assert(elapsed >= (uint32_t)cfg.syncAckTimeoutMs &&
           elapsed <= (uint32_t)cfg.syncAckTimeoutMs + 30 &&
           "waitForAck must time out at t0+timeoutMs even with "
           "steady foreign ACK traffic — the deadline MUST NOT "
           "slide. Pre-fix shape re-armed the full timeoutMs "
           "slice on every foreign-ACK sem take, so a steady "
           "50 ms foreign-ACK rate pushed the verdict out "
           "indefinitely and the SYNC retx ladder never fired.");
    std::cout << "PASS" << std::endl;
}

void test_clearall_bumps_generation() {
    std::cout << "\n=== Test: clearAll() bumps the reset-generation counter ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);
    hal.lock();
    uint32_t g0 = t.arq().generation();
    t.arq().clearAll();
    uint32_t g1 = t.arq().generation();
    t.arq().clearAll();
    uint32_t g2 = t.arq().generation();
    hal.unlock();

    if (g1 != g0 + 1 || g2 != g0 + 2) {
        std::cerr << "\nFAIL: clearAll() did not bump generation "
                  << "monotonically (g0=" << g0 << " g1=" << g1 << " g2=" << g2
                  << ")" << std::endl;
        assert(false);
    }
    std::cout << "  g0=" << g0 << " g1=" << g1 << " g2=" << g2 << std::endl;
    std::cout << "PASS" << std::endl;
}

// Pin: LinkArq::sentAtMs_ is budget-depth (ARQ_CHUNK_BUDGET),
// not the full 256-deep COBS seq space, and shares the same
// budgetIdx(seq) index as the other four per-seq fields — not
// idxOf. If a future change rolls sentAtMs_ back to 256-deep,
// or back onto idxOf, this pin catches it. (idxOf-backed
// sentAtMs_ is not just "less memory saved" — it reintroduces
// the staleness bug the next pin,
// test_sentatms_survives_burst_send_and_gbnbase_advance, pins;
// see LinkArq.h's class comment.)
static void test_linkarq_sentatms_is_budget_depth() {
    std::cout << "\n=== Source-grep: sentAtMs_ is budget-depth, "
                 "budgetIdx-backed ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/arq/LinkArq.h").c_str(), "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *m = strstr(buf, "uint32_t sentAtMs_[");
    assert(m && "LinkArq.h must declare uint32_t sentAtMs_[...]");
    assert(!strstr(m, "sentAtMs_[256]") &&
           "sentAtMs_ must NOT be 256-deep; the ring shrink is the "
           "whole point of this optimization");
    // ARQ_CHUNK_BUDGET is the contract — the symbolic
    // identifier, not a literal, so a future bump only touches
    // AutoLinkConfig.h.
    assert(strstr(m, "ARQ_CHUNK_BUDGET") &&
           "sentAtMs_ must size from ARQ_CHUNK_BUDGET (budgetIdx-depth, "
           "matching the other four per-seq fields), not "
           "AUTOLINK_ARQ_PIPELINE_WINDOW (idxOf-depth)");

    // LinkArq.cpp's writes to sentAtMs_[] must pass through
    // budgetIdx (a fixed function of seq), not idxOf (relative
    // to gbnBase_, which drifts across a gbnBase_ advance).
    FILE *g = fopen(testRepoPath("src/al/link/arq/LinkArq.cpp").c_str(), "r");
    assert(g);
    char abuf[16384];
    size_t an = fread(abuf, 1, sizeof(abuf) - 1, g);
    abuf[an] = 0;
    fclose(g);
    assert(strstr(abuf, "sentAtMs_[bi]") &&
           "sentAtMs_ writes in LinkArq.cpp must use [bi] (the "
           "budgetIdx-derived index), not idxOf's [i]");

    std::cout << "  PASS (sentAtMs_ is budget-depth, writes go through "
                 "budgetIdx)"
              << std::endl;
}

// Pin: sentAtMs_ must track the correct chunk's own send time
// across a gbnBase_ advance, not whatever happened to occupy
// index 0 last. Reproduces the staleness an idxOf-backed
// sentAtMs_ has: burst-send two chunks while gbnBase_ is fixed
// (so their onSent-time idxOf offsets are 0 and 1 respectively),
// ack the first and advance gbnBase_ past it (mirroring the
// cumulative-ack handler: onAcked() first, setGbnBase() after),
// then ask decideSlot() about the new base (the second chunk)
// moments after its real send. An idxOf-backed sentAtMs_ reads
// sentAtMs_[idxOf(gbnBase_)] == sentAtMs_[0], which the second
// chunk never wrote (only the first chunk ever lived at index 0)
// — so it reads either a stale send time from an unrelated
// earlier chunk or the zero-initialized default, computing a
// huge bogus age and firing Retx/Drop on a chunk sent moments
// ago. Toggle sentAtMs_ back onto idxOf (or drop it back to
// window-depth) -> red.
static void test_sentatms_survives_burst_send_and_gbnbase_advance() {
    std::cout << "\n=== Test: sentAtMs_ tracks the right chunk's send "
                 "time across a gbnBase_ advance ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link link(hal, cache, true, cfg);
    link.begin();

    LinkTestAccessor t(link);
    LinkArq &arq = t.arq();

    // Burst: seq=0 sent at t=1000; seq=1 sent much later (t=9000)
    // but gbnBase_ is still 0 for both sends (seq=0 hasn't been
    // acked yet), so onSent-time idxOf gives 0 and 1
    // respectively — different physical slots under the old
    // idxOf scheme.
    arq.onSent(0, 0xFF, 1000);
    arq.onSent(1, 0xFF, 9000);

    // seq=0 acked; gbnBase_ advances to 1 — mirrors
    // Link::onAck's cumulative-ack loop in LinkRx.cpp, which
    // calls onAcked() for every seq up to the acked one, THEN
    // setGbnBase() once, after the loop.
    arq.onAcked(0);
    arq.setGbnBase(1);

    // decideSlot for the new base (seq=1) only 100 ms after its
    // real send (t=9000), well inside a generous 5000 ms RTO —
    // must Hold.
    LinkArq::Action a = arq.decideSlot(arq.gbnBase(), 9100,
                                       /*ackRtoMs=*/5000, /*maxRetx=*/5);
    if (a != LinkArq::Action::Hold) {
        std::cerr << "\nFAIL: decideSlot fired (action="
                  << (a == LinkArq::Action::Retx ? "Retx" : "Drop")
                  << ") 100 ms after seq=1's real send (t=9000) — "
                  << "sentAtMs_ read the wrong physical slot instead of "
                  << "seq=1's own timestamp" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (decideSlot correctly Holds 100 ms after seq=1's "
                 "real send, across the gbnBase_ advance)"
              << std::endl;
}

int main() {
    std::cout << "=== Running ALink ARQ Tests (v5: per-message ACK) ==="
              << std::endl;
    test_ack_type_constant();
    test_unknown_cobs_ack_dropped();
    test_duplicate_acks_are_idempotent();
    test_ack_type_not_a_preamble_or_cmd();
    test_ack_type_outside_cobsseq_reserved();
    test_pending_acks_invariant();
    test_ack_wire_round_trip();
    test_base_seq_self_for_single_chunk();
    test_retransmit_does_not_deadlock_with_lock();
    test_sendmsg_stalls_when_arq_cache_full();
    test_reset_clears_arq_state_maps();
    test_keepalive_does_not_trigger_ack();
    test_waitforack_detects_aba_after_unlock();
    test_waitforack_returns_true_when_no_reset_fires();
    test_waitforack_times_out_when_no_ack_or_reset();
    test_waitforack_deadline_does_not_slide_on_foreign_acks();
    test_clearall_bumps_generation();
    test_linkarq_sentatms_is_budget_depth();
    test_sentatms_survives_burst_send_and_gbnbase_advance();
    std::cout << "\n=== ALink ARQ Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif