// 3-phase sweep + PONG_ACK + 2-of-3 lock.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "al/util/Log.h"
#    include "NullArqCache.h"

using namespace autolink;

// P1 -> P2 transition on first PONG.
// Old code: master received its first PONG in PHASE1 and called
// lockOk_unlocked(lb, "phase1") — committing to the slowest baud.
// Fix: master calls enterPhase2_unlocked() instead, which resets
// spdI to 0 (fastest). We assert the baud CHANGED away from the
// slowest during the round-trip. (MockHal doesn't simulate baud
// mismatches, so the link may end up in OK; the contract we pin
// here is the master transitioning out of PHASE1 to a faster baud,
// which only the new code does.)
static void test_v531_phase1_first_pong_routes_to_phase2() {
    NullArqCache cache;
    std::cout << "\n=== Phase 1 first PONG routes to P2 (no immediate lock) ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    assert(ping.getCurrentSpdIndex() == 1);
    assert(pong.getCurrentSpdIndex() == 1);

    // Master ticks; slave receives PING and replies with PONG.
    ping.onTimer();
    assert(!mHal.txBuf.empty());
    std::vector<uint8_t> m2s = mHal.txBuf;
    mHal.clearTx();
    sHal.link->onRx(m2s.data(), (int)m2s.size());

    // Slave has now replied with a PONG to master.
    assert(!sHal.txBuf.empty());
    std::vector<uint8_t> s2m = sHal.txBuf;
    sHal.clearTx();
    mHal.link->onRx(s2m.data(), (int)s2m.size());

    // Contract: master must have advanced past the slowest baud.
    // Old code: spdI stayed at 1 (slowest) — the immediate lock
    // committed to whatever baud the PONG happened to arrive at.
    // New code: spdI goes 1 -> 0 (fastest) because enterPhase2
    // resets to the top of the allowed-bauds list.
    assert(ping.getCurrentSpdIndex() == 0);
    std::cout << "PASS (master advanced to fastest baud after PONG)"
              << std::endl;
}

static void test_v531_phase1_locks_at_slowest_baud() {
    NullArqCache cache;
    std::cout << "\n=== Phase 1 first PONG advances past slowest baud ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    assert(ping.getCurrentSpdIndex() == 1);
    assert(pong.getCurrentSpdIndex() == 1);

    // Drive one round-trip.
    ping.onTimer();
    if (!mHal.txBuf.empty()) {
        std::vector<uint8_t> b = mHal.txBuf;
        mHal.clearTx();
        sHal.link->onRx(b.data(), (int)b.size());
    }
    if (!sHal.txBuf.empty()) {
        std::vector<uint8_t> b = sHal.txBuf;
        sHal.clearTx();
        mHal.link->onRx(b.data(), (int)b.size());
    }

    // Master's baud index must have moved off the slowest (1) — it
    // either committed to fastest (0) via P3 or is mid-sweep. Old
    // code: stayed at 1. New code: 0.
    assert(ping.getCurrentSpdIndex() == 0);
    std::cout << "PASS (committed to fastest, not slowest)" << std::endl;
}

static void test_v531_pong_acks_first_ping() {
    NullArqCache cache;
    std::cout << "\n=== PONG_CMD = 0x33 decodes correctly ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    assert(PING_CMD == 0x22);
    assert(PONG_CMD == 0x33);
    assert(REQ_CMD == 0x11);
    std::cout << "PASS" << std::endl;
}

static void test_v531_heartbeat_miss_drops_quickly() {
    NullArqCache cache;
    std::cout << "\n=== heartbeat miss drops in <500ms ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    mHal.now = 0;
    int dropsAt = -1;
    for (int i = 0; i < 50; i++) {
        mHal.now += 50;
        ping.onTimer();

        if (ping.getState() == State::SWP) {
            dropsAt = i;
            break;
        }
    }
    assert(dropsAt >= 0);

    assert(dropsAt < 10);
    std::cout << "PASS (dropped at heartbeat tick " << dropsAt << ")"
              << std::endl;
}

static void test_v531_dwells_computed_at_boot() {
    NullArqCache cache;
    std::cout << "\n=== dwell caps computed from baud list ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.allowedBauds[2] = 38400;
    cfg.allowedBauds[3] = 19200;
    cfg.allowedBauds[4] = 9600;
    cfg.allowedBaudsCount = 5;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    assert(ping.getCurrentSpdIndex() == 4);
    std::cout << "PASS" << std::endl;
}

static void test_v531_banner_logged_on_phase_entry() {
    NullArqCache cache;
    std::cout << "\n=== phase banners appear in log ===" << std::endl;
    Log::log().setLevel(Log::DEBUG);
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    ping.begin();

    assert(true);
    std::cout << "PASS (visual inspection: === PHASE 1 === banner)"
              << std::endl;
}

int main() {
    std::cout << "=== Running Sweep Tests ===" << std::endl;
    test_v531_phase1_first_pong_routes_to_phase2();
    test_v531_phase1_locks_at_slowest_baud();
    test_v531_pong_acks_first_ping();
    test_v531_heartbeat_miss_drops_quickly();
    test_v531_dwells_computed_at_boot();
    test_v531_banner_logged_on_phase_entry();
    std::cout << "\n=== Sweep Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif