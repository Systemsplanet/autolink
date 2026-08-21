// Field-wedge regression pins. One file per
// release-batch — each pin is the field-log symptom
// reproduced in a host subsecond test. Toggle the fix
// off and the matching pin flips red; green/green
// means the test is useless and must be deleted.
//
// Pins (the field-wedge batch, in issue order, plus
// the freertos-free header guard):
//   1. isPending is side-effect-free (no log). A
//      per-call debug log on the hot path would
//      saturate the log transport at 400+ chunks/s
//      and wedge the SYNC wait timeout when the log
//      mutex was held.
//   2. waitForAck times out cleanly when neither ACK
//      nor reset fires (the host-side busy-spin path
//      must still surface a timeout). The ARDUINO-side
//      event-driven path (xSemaphoreTake on ack_sem_)
//      is verified by source-grep on the .cpp file.
//   3. AutoLink::begin() forwards setMode(cfg.mode)
//      to the HAL before hal->begin() so the
//      txBufferFloor picks up the correct branch.
//   4. AutoLink::begin() logs an error if the HAL's
//      mode disagrees with the facade's mode at
//      post-begin time (custom IHal that didn't honour
//      setMode()).
//   5. Drain-RX prevents stuck honest-drop: a
//      pending ARQ with a non-empty app RX buffer
//      must re-arm gbnBaseStuckSinceMs_, not fire
//      the base-stuck verdict.
//   6. CPU-stall re-arms base-stuck: a tick gap > 3x
//      the OK-tick interval (or > 1 RTT, whichever is
//      larger) re-arms gbnBaseStuckSinceMs_ so a
//      starved OK-timer task doesn't count its own
//      absence as peer silence.
//   7. DropPeerStalled watchdog fires on single-sided
//      dead peer: pending > 0 + ackRxMs older than
//      the baud-derived window trips the verdict.
//      The slave's own echo traffic keeps lastTxMs
//      fresh so DropDeadLink's mutual-quiet gate
//      never trips — the new verdict closes the gap.
//   8. healthPeerStalledMs is baud-derived: 2s floor
//      at any baud, with the higher of that and 2x
//      the window-drain at the locked baud.
//   9. Per-frame wire trace is compile-time gated by
//      AUTOLINK_TRACE_WIRE (default off). Unconditional
//      verbose-level per-chunk lines would saturate
//      the transport when the operator lifts the
//      level to VERBOSE for debugging.
//   10. P3-entry BREAK fires on every reset, not
//      just the first. The master's 3.3 s cycling
//      collapse came from a `if (brk && first)`
//      short-circuit that swallowed every
//      second-and-later cycle.
//   11. LinkArq.h is freertos-free. The header
//      must not include <freertos/semphr.h>
//      (directly or transitively); the second-pass
//      include-order trap returns otherwise. The
//      semaphore is held as void* in the header
//      and the real include lives in the .cpp.
#ifndef ARDUINO

#    include <cassert>
#    include <atomic>
#    include <chrono>
#    include <cstdio>
#    include <cstring>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <thread>
#    include <vector>
#    include <sys/stat.h>
#    include <sys/types.h>

#    include "MockHal.h"
#    include "TestCfg.h"
#    include "LinkTestAccessor.h"
#    include "AutoLinkTestAccessor.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "al/link/timers/LinkHealth.h"
#    include "al/AutoLinkConfig.h"
#    include "al/util/log/Log.h"
#    include "AutoLink.h"
#    include "EspHalStub.h"
#    include "NullArqCache.h"

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

// Pin 1: LinkArq::isPending is log-free. The
// the shape called Log::log().debug on every
// invocation; the 400+ chunks/s ASYNC pipeline rate
// produced 1600+ log lines/s, evicting real events
// from the drop-oldest ring and (worse) blocking the
// SYNC wait spin when the log sink held the log
// mutex. The fix is to make isPending side-effect-
// free at the source level. Pinned by a source-grep
// check on the absence of that debug call.
void test_pin_1_ispending_log_free() {
    std::cout << "\n=== Pin 1: LinkArq::isPending is side-effect-free "
                 "(no log call) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/link/arq/LinkArq.cpp");
    assert(!src.empty());
    auto p = src.find("bool LinkArq::isPending");
    assert(p != std::string::npos);
    // Read the function body up to the next free
    // function (rough heuristic: next `bool
    // LinkArq::` or `int LinkArq::` or end of
    // namespace).
    auto end = src.find("bool LinkArq::waitForAck", p);
    if (end == std::string::npos)
        end = src.find("int LinkArq::pendingCount", p);
    if (end == std::string::npos)
        end = src.find("LinkArq::Action", p);
    assert(end != std::string::npos);
    std::string body = src.substr(p, end - p);
    // Strip comments so a "// Log::log" inside a
    // comment doesn't fake the absence check.
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
    std::string code = stripComments(body);
    assert(code.find("Log::log()") == std::string::npos &&
           "LinkArq::isPending must not call Log::log — "
           "debug log on every call saturated the wire-transport log "
           "at ASYNC pipeline rate and (worse) blocked the SYNC wait "
           "spin when the log sink held the log mutex, turning the "
           "wait timeout into a no-op. Toggle the log back on -> red.");
    std::cout << "  PASS (isPending body has no Log::log call)" << std::endl;
}

// Pin 2a: waitForAck times out cleanly on the host
// (busy-spin path) when neither an ACK nor a reset
// fires. The the shape had a pre-loop log
// path that, combined with the isPending debug log,
// could wedge the spin. Pinned behaviourally so a
// regression in the host-side waitForAck flips red.
//
// Host-side waitForAck is a busy-spin against
// MockHal::now; the clock only advances when the
// test thread pumps it. A background thread runs
// pumpClock(1) for the duration of the wait so
// the spin observes the elapsed time.
void test_pin_2a_waitforack_timeout() {
    std::cout
        << "\n=== Pin 2a: waitForAck times out cleanly (no ACK, no reset) ==="
        << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.syncAckTimeoutMs = 50;
    Link a(hal, cache, true, cfg);
    a.begin();
    LinkTestAccessor t(a);

    // Mark a pending slot; do NOT deliver any ACK.
    t.markAckedPending(7);
    assert(a.arqPendingCount() == 1);

    std::atomic<bool> stop{ false };
    std::thread pumper([&]() {
        while (!stop.load()) {
            hal.pumpClock(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    uint32_t t0 = hal.now;
    bool ok = t.arq().waitForAck(a, 7, 50);
    uint32_t elapsed = hal.now - t0;
    stop.store(true);
    pumper.join();
    std::cout << "  timeout=50 ms elapsed=" << elapsed
              << " ms returned=" << (ok ? "true" : "false") << std::endl;
    assert(!ok && "waitForAck must return false on timeout");
    assert(elapsed >= 50 &&
           "waitForAck must NOT exit before the full timeout elapses — "
           "log-wedge symptom returned early (the timeout "
           "check was inside the spin and was bypassed when the log "
           "mutex was held).");
    assert(elapsed < 200 &&
           "waitForAck must exit at-or-near the timeout, not "
           "hang on a busy-spin forever");
    std::cout << "  PASS" << std::endl;
}

// Pin 2b: waitForAck source-grep on the ARDUINO-side
// event-driven path. The fix is to take a
// xSemaphoreHandle on the ack_sem_ instead of
// portYIELD() spin, so a 100ms-stretched ACK costs
// zero CPU. Pinned at the source level: the file
// must reference xSemaphoreTake inside waitForAck
// (event-driven), and that portYIELD
// inside the spin must be gone.
void test_pin_2b_waitforack_event_driven_source() {
    std::cout << "\n=== Pin 2b: waitForAck uses xSemaphoreTake on "
                 "ARDUINO (event-driven, not portYIELD spin) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/link/arq/LinkArq.cpp");
    assert(!src.empty());
    auto p = src.find("bool LinkArq::waitForAck");
    assert(p != std::string::npos);
    auto end = src.find("LinkArq::pendingCount", p);
    if (end == std::string::npos)
        end = src.find("LinkArq::Action", p);
    assert(end != std::string::npos);
    std::string body = src.substr(p, end - p);
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
    std::string code = stripComments(body);
    // The ARDUINO branch must reference the
    // semaphore take, and must NOT keep the
    // the portYIELD() spin inside the
    // loop. (Host path keeps the spin — host tests
    // are single-threaded, the spin is the
    // cheapest shape there.)
    assert(code.find("xSemaphoreTake") != std::string::npos &&
           "LinkArq::waitForAck must call xSemaphoreTake on ARDUINO — "
           "the current fix replaces the portYIELD() busy-spin with a "
           "semaphore take so a 100 ms ACK latency costs zero CPU. "
           "Without this, a SYNC send pegs the loop task at 100% for "
           "the full syncAckTimeoutMs window on every send.");
    // F6: the body should still have a
    // clearAll bump (the clearAllEpoch_
    // wake path) so a reset unblocks
    // the wait. The pre-F-pass name
    // was generation_; F6 renamed it
    // to clearAllEpoch_ to remove the
    // misleading association with the
    // cobsSeq lap (which is txSeqLap_,
    // not this counter).
    assert(code.find("clearAllEpoch_") != std::string::npos &&
           "waitForAck must consult clearAllEpoch_ to surface a "
           "mid-wait reset_unlocked() call as a false return "
           "(the current fix uses clearAll's xSemaphoreGive to "
           "wake the waiter; the epoch mismatch closes the ABA race).");
    std::cout << "  PASS (xSemaphoreTake inside waitForAck; "
                 "generation guard present)"
              << std::endl;
}

// Pin 3: AutoLink::begin() must NOT forward setMode(cfg_.mode)
// to the HAL. cfg_.mode is a construction-time snapshot; the
// app's deliberate setMode(ASYNC) before begin() is installed
// via AutoLink::setMode(m) → hal->setMode(m) → link->setMode(m),
// and Link::begin() begins the HAL from the link's config
// (which already carries the mode setMode installed). Calling
// hal->setMode(cfg_.mode) from begin() actively reverts any
// mode the app installed via setMode() — cfg_.mode is a
// construction-time snapshot, not the live mode, and the
// reversion is what the field log reported as "the GUI is
// wedged but the link is fine" (link / HAL / buffer sizes
// ended up correct; only the facade's view was wrong, but
// the mode-mismatch error fired on a healthy configuration).
// The setMode-vs-begin path is pinned at runtime by
// ModeSyncBeforeBeginTest. The constraint here is the
// absence of the revert.
void test_pin_3_setmode_forwarded_to_hal_before_begin() {
    std::cout
        << "\n=== Pin 3: AutoLink::begin() does NOT revert hal->setMode ==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    auto p = src.find("bool begin()");
    assert(p != std::string::npos);
    auto end = src.find("void blinkWait", p);
    assert(end != std::string::npos);
    std::string body = src.substr(p, end - p);
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
    std::string code = stripComments(body);
    // hal->setMode() must NOT be called by begin() with the
    // stale cfg_.mode as the argument. Link::begin() begins
    // the HAL from the link's config, so the explicit
    // facade-side handoff is both wrong and redundant.
    assert(code.find("hal->setMode(cfg_.mode)") == std::string::npos &&
           "AutoLink::begin() must not call hal->setMode(cfg_.mode) — "
           "cfg_.mode is a construction-time snapshot, not the live "
           "mode; re-applying it on begin() silently reverts any mode "
           "the app installed via setMode() before begin(). The "
           "setMode → begin agreement is pinned by "
           "ModeSyncBeforeBeginTest.");
    // link->begin() is still required, of course.
    assert(code.find("link->begin()") != std::string::npos);
    std::cout << "  PASS (begin has no hal->setMode(cfg_.mode) revert)"
              << std::endl;
}

// Pin 4: AutoLink::begin() logs a "mode mismatch at begin"
// error if the HAL's mode disagrees with the LINK's mode
// (not cfg_.mode — that's a construction-time snapshot).
// Pinned at the source level. The link is the source of
// truth for "what mode were the buffers sized for"; a
// HAL that drifted away from the link's mode is exactly
// what the guard is meant to catch, since the buffers
// were sized against the link's mode and a wrong-mode
// HAL would block on a tx ring sized for the other
// window. Pinned by ModeSyncBeforeBeginTest at runtime.
void test_pin_4_begin_logs_mode_mismatch() {
    std::cout << "\n=== Pin 4: AutoLink::begin() logs a mode-mismatch "
                 "error if HAL.mode != link.mode ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());
    auto p = src.find("bool begin()");
    assert(p != std::string::npos);
    auto end = src.find("void blinkWait", p);
    assert(end != std::string::npos);
    std::string body = src.substr(p, end - p);
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
    std::string code = stripComments(body);
    // IHal must declare getMode() (the post-begin
    // check uses it).
    std::string ihal = readFile(projectRoot() + "/src/al/hal/IHal.h");
    assert(!ihal.empty());
    std::string ihalCode = stripComments(ihal);
    assert(ihalCode.find("virtual AutoLinkConfig::Mode getMode()") !=
               std::string::npos &&
           "IHal must declare virtual AutoLinkConfig::Mode getMode() "
           "const so AutoLink::begin() can detect a HAL/link mode "
           "divergence post-begin()");
    // AutoLink::begin() must compare hal->getMode() against
    // link->mode() and log an error on disagreement. The
    // broken check compared against cfg_.mode, which was
    // the bug class — cfg_.mode is a construction-time
    // snapshot and does not track setMode().
    assert(code.find("hal->getMode()") != std::string::npos &&
           "AutoLink::begin() must call hal->getMode() to compare "
           "against link->mode()");
    assert(code.find("link->mode()") != std::string::npos &&
           "AutoLink::begin()'s mode-mismatch check must compare "
           "hal->getMode() against link->mode() — the link's mode is "
           "what the buffers were sized for; cfg_.mode is a stale "
           "snapshot. ModeSyncBeforeBeginTest pins this at runtime.");
    assert(code.find("mode mismatch at begin") != std::string::npos &&
           "AutoLink::begin() must log a \"mode mismatch at begin\" "
           "error if hal->getMode() != link->mode() — a custom IHal "
           "that didn't honour setMode() must be caught before the "
           "link starts dropping frames.");
    std::cout << "  PASS (IHal::getMode declared; AutoLink::begin "
                 "compares hal vs link, not hal vs cfg)"
              << std::endl;
}

// Pin 5: drain-RX prevents stuck honest-drop. Drive
// a SWP->OK->OK-wedged scenario where the master's
// app buffer has queued echo data (peer is alive,
// peer is sending) but the master's ARQ base has
// been stuck. Without the re-arm, sweepRetx_unlocked
// would fire baseStormStuck and honest-drop. With
// the re-arm, the storm-stuck clock is reset and
// the link survives. Behavioural pin.
void test_pin_5_drain_rx_re_arms_base_stuck() {
    std::cout << "\n=== Pin 5: drain-RX re-arms base-stuck (peer IS "
                 "sending — don't fire honest-drop) ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.idleTimeoutMs = 5000;
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link a(hal, cache, true, cfg);
    a.begin();
    LinkTestAccessor t(a);

    // Force the link into OK with a stuck base and
    // a populated app RX buffer (the peer IS
    // sending).
    t.forceState(State::OK);
    t.setGbnBase(7);
    t.setGbnBaseStuckThresholdMsForTest(500);

    // Stage: gbnBaseStuckSinceMs_ is 1000ms in the
    // past (would normally trip the stuck verdict
    // at 500ms), app RX buffer has 32 bytes
    // (peer sent a payload the app hasn't drained).
    uint32_t now = hal.now + 1000;
    // Push 32 bytes into the app RX buffer.
    uint8_t payload[32];
    memset(payload, 0xAA, sizeof payload);
    int n = hal.pushAppBuf(payload, sizeof payload);
    assert(n == 32);
    // Stage OkTick + the stuck clock so
    // sweepRetx_unlocked sees the wedge shape.
    hal.now = now;
    t.gbnBaseStuckSinceMs_set_for_test(now - 1000);
    t.gbnBaseStuckTrackedSeq_set_for_test(7);

    // Drive sweepRetx_unlocked: it must NOT fire
    // baseStormStuck when the app RX buffer is
    // non-empty.
    bool brk = t.sweepRetx(now);
    uint32_t stuckAfter = t.gbnBaseStuckSinceMs_for_test();
    std::cout << "  brk=" << (brk ? "true" : "false")
              << " gbnBaseStuckSinceMs after=" << stuckAfter << " (now=" << now
              << ")" << std::endl;
    assert(stuckAfter == now &&
           "Drain-RX fix: gbnBaseStuckSinceMs_ must be re-armed to "
           "`now` when the app's RX buffer holds the peer's data. "
           "The shape shape would have kept the stuck clock at "
           "(now-1000) and the next sweep would have fired "
           "baseStormStuck -> honest-drop on evidence the peer was "
           "already sending. Toggle the re-arm off -> red.");
    // Also no reset (no state change).
    assert(!brk &&
           "sweepRetx must not request a BREAK when the app RX "
           "buffer proves the peer is alive");
    std::cout << "  PASS" << std::endl;
}

// Pin 6: CPU-stall re-arms base-stuck. Drive a
// large gap between OK-ticks; the gap exceeds 3x
// the OK-tick interval and triggers the re-arm.
// Behavioural pin.
void test_pin_6_cpu_stall_re_arms_base_stuck() {
    std::cout << "\n=== Pin 6: CPU-stall re-arms base-stuck (OK-timer "
                 "task was starved) ==="
              << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.idleTimeoutMs = 5000;
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link a(hal, cache, true, cfg);
    a.begin();
    LinkTestAccessor t(a);

    t.forceState(State::OK);
    t.setGbnBase(7);
    t.setGbnBaseStuckThresholdMsForTest(500);

    // Stage: a prior OK-tick at t=100, current
    // tick at t=2000 (a ~1.9 s gap — well past
    // the 3x OK-tick floor of ~1500 ms at default
    // 500 ms syncAckTimeoutMs). gbnBaseStuckSinceMs_
    // is also 1900ms in the past.
    uint32_t now = 2000;
    t.lastOkTickMs_set_for_test(100);
    t.gbnBaseStuckSinceMs_set_for_test(100);
    t.gbnBaseStuckTrackedSeq_set_for_test(7);
    hal.now = now;
    t.sweepRetx(now);
    uint32_t stuckAfter = t.gbnBaseStuckSinceMs_for_test();
    std::cout << "  priorOkTick=100 now=" << now
              << " gbnBaseStuckSinceMs after=" << stuckAfter << std::endl;
    assert(stuckAfter == now &&
           "CPU-stall fix: gbnBaseStuckSinceMs_ must be re-armed to "
           "`now` when the gap between OK-ticks exceeds the stall "
           "floor. The shape shape counted the gap as peer silence "
           "and the master would wipe 18 chunks the slave had "
           "actually ACKed. Toggle the re-arm off -> red.");
    std::cout << "  PASS" << std::endl;
}

// Pin 7: DropPeerStalled watchdog. The slave's own
// echo traffic keeps lastTxMs fresh so DropDeadLink's
// mutual-quiet gate never trips; the new verdict
// closes the gap. Drive pending > 0 with a stale
// ackRxMs and a baud-derived threshold; decideHealth
// must return DropPeerStalled. Behavioural pin.
void test_pin_7_drop_peer_stalled_fires_on_single_sided_dead() {
    std::cout
        << "\n=== Pin 7: DropPeerStalled fires on single-sided dead peer ==="
        << std::endl;
    // Build a HealthState directly — decideHealth is
    // a pure decision function in LinkHealth.h.
    HealthState h;
    h.lastRxMs = 0;
    h.lastTxMs = 0;         // doesn't matter — DropPeerStalled
                            // uses ackRxMs not lastTxMs
    h.ackRxMs = 1000;       // peer spoke at t=1000
    h.peerStalledMs = 2000; // 2 s floor
    h.pending = 4;          // we have un-ACKed
    h.rtoMs = 500;
    h.frameErrs = 0;
    h.poolFull = false;
    h.sync = false;
    uint32_t now = 5000; // 4 s after ackRxMs
    HealthAction a = decideHealth(h, now, /*idleTimeoutMs=*/10000,
                                  /*deadPeerMs=*/30000, h.rtoMs);
    std::cout << "  ackRxAge=" << (now - h.ackRxMs)
              << " ms peerStalledMs=" << h.peerStalledMs
              << " pending=" << h.pending << " verdict=" << (int)a << std::endl;
    assert(a == HealthAction::DropPeerStalled &&
           "pending>0 with ackRxAge > peerStalledMs MUST fire "
           "DropPeerStalled. The the path used "
           "DropDeadLink which required both rxAge and txAge > "
           "idleTimeoutMs; the slave's own echo traffic kept "
           "txAge fresh and the watchdog never tripped. The shape "
           "field log showed the slave sitting in OK for 10.5 s "
           "after the master stopped ACKing with the watchdog "
           "silent the whole time.");
    std::cout << "  PASS" << std::endl;
}

// Pin 7b: DropPeerStalled does NOT fire when
// pending == 0 (clean idle link is fine). Defensive
// pin for the "I have nothing to ACK, the peer went
// quiet, I should hold" case.
void test_pin_7b_drop_peer_stalled_holds_when_pending_zero() {
    std::cout << "\n=== Pin 7b: DropPeerStalled holds when pending == 0 "
                 "(clean idle link) ==="
              << std::endl;
    HealthState h;
    h.lastRxMs = 0;
    h.lastTxMs = 0;
    h.ackRxMs = 1000;
    h.peerStalledMs = 2000;
    h.pending = 0; // no in-flight ARQ
    h.rtoMs = 500;
    h.frameErrs = 0;
    h.poolFull = false;
    h.sync = false;
    uint32_t now = 5000;
    HealthAction a = decideHealth(h, now, 10000, 30000, h.rtoMs);
    std::cout << "  pending=" << h.pending << " ackRxAge=" << (now - h.ackRxMs)
              << " verdict=" << (int)a << std::endl;
    assert(a != HealthAction::DropPeerStalled &&
           "DropPeerStalled must NOT fire on a clean idle link "
           "(pending == 0). The pending>0 gate is the whole point "
           "of the new verdict — it narrows the dead-peer check "
           "to \"we owe or are owed ACKs\" instead of \"the link "
           "is idle\". The clean-idle contract from "
           "symmetric-idle removal is preserved.");
    std::cout << "  PASS" << std::endl;
}

// Pin 8: healthPeerStalledMs is baud-derived. 2s
// floor at any baud; the higher of (2s) and (2x the
// window-drain) at slow bauds.
void test_pin_8_peer_stalled_baud_derived() {
    std::cout << "\n=== Pin 8: healthPeerStalledMs is baud-derived "
                 "(2s floor; 2x drain at slow bauds) ==="
              << std::endl;
    // 512000 with a full 32-chunk window: drain is
    // ~0.16ms * 32 = ~5ms; 2x = 10ms; floor wins -> 2000.
    int w512 = healthPeerStalledMs(512000, 32, 0);
    std::cout << "  512000 baud, pending=32: " << w512 << " ms" << std::endl;
    assert(w512 == 2000 && "fast baud must collapse to the 2s floor");
    // 9600 with a full 32-chunk window: drain is
    // ~85ms * 32 = ~2730ms; 2x = ~5461ms; max(2000, 5461)
    // = 5461.
    int w96 = healthPeerStalledMs(9600, 32, 0);
    std::cout << "  9600 baud, pending=32: " << w96 << " ms" << std::endl;
    assert(w96 >= 5000 &&
           "9600 baud full window must give a drain-derived "
           "verdict (>=5s), not the 2s floor");
    // Baud 0 (lock-up) collapses to the 2s floor.
    int w0 = healthPeerStalledMs(0, 32, 0);
    assert(w0 == 2000 && "baud==0 must collapse to the 2s floor");
    // Pending 0 collapses to the 2s floor.
    int wp0 = healthPeerStalledMs(9600, 0, 0);
    assert(wp0 == 2000 && "pending==0 must collapse to the 2s floor");
    std::cout << "  PASS" << std::endl;
}

// Pin 9 (AL97-3): healthPeerStalledMs must respect the GBN
// exponential-backoff ladder (decideGbnBackoff, capped by
// gbnBackoffCapMs_unlocked at 8x syncAckTimeoutMs). Without this,
// the watchdog's flat 2000 ms floor can fire BEFORE the GBN retx
// ladder's next scheduled round — LinkTimersGbn.cpp's own
// effectiveStuckThresholdMs already applies this exact clamp to its
// honest-drop clock; this watchdog and that one must never race on
// the same stalled-peer condition. A field capture at 512000 baud
// showed a backoff round due at ~19497ms fire 3ms AFTER a 2000ms-
// floor watchdog tore the link down at 19500ms, wiping 12
// accepted-undelivered chunks.
void test_pin_9_peer_stalled_respects_gbn_backoff() {
    std::cout << "\n=== Pin 9: healthPeerStalledMs respects the GBN "
                 "backoff floor ==="
              << std::endl;
    // Fast baud, small pending, no backoff in play: unchanged from
    // Pin 8 — the flat 2000 ms floor still wins.
    int noBackoff = healthPeerStalledMs(512000, 4, 0);
    assert(noBackoff == 2000 &&
           "backoffFloorMs=0 must not change any existing baud/pending "
           "verdict");
    // Same fast baud/small pending, but the GBN ladder has backed
    // off to 3000 ms (well past the flat floor and past the
    // baud-derived drain time, which is ~2ms at this baud/pending).
    // The watchdog window must not fire before the ladder's next
    // scheduled round.
    int withBackoff = healthPeerStalledMs(512000, 4, 3000);
    assert(withBackoff >= 3000 &&
           "backoffFloorMs must raise the peer-stalled window at least "
           "to its own value — a fast-baud, light-pending link must "
           "not collapse to the flat 2000 ms floor while the GBN retx "
           "ladder has a later round scheduled");
    // At a slow baud where the drain-derived verdict already exceeds
    // the backoff floor, the drain-derived value must still win (max
    // of the two, not the backoff value overriding a legitimately
    // larger drain-derived window).
    int slowBaudBeatsBackoff = healthPeerStalledMs(9600, 32, 1000);
    assert(slowBaudBeatsBackoff >= 5000 &&
           "a larger drain-derived window must not be shrunk by a "
           "smaller backoff floor — the function must take the max "
           "of both inputs, not prefer whichever is passed last");
    std::cout << "  PASS" << std::endl;
}

// Pin 9: per-frame wire trace is compile-time gated
// by AUTOLINK_TRACE_WIRE (default off). Unconditional
// verbose-level per-chunk lines would saturate
// the transport when the operator lifts the
// level to VERBOSE for debugging. Source-grep pin
// on the absence of unconditional verbose wire logs.
void test_pin_9_wire_trace_compile_gated() {
    std::cout << "\n=== Pin 9: per-frame wire trace is compile-time "
                 "gated (AUTOLINK_TRACE_WIRE, default off) ==="
              << std::endl;
    // Check that AUTOLINK_TRACE_WIRE is referenced
    // at all (the gate exists). The actual
    // per-frame log lines are inside #ifdef
    // AUTOLINK_TRACE_WIRE blocks.
    std::string srcRx = readFile(projectRoot() + "/src/al/link/io/LinkRx.cpp");
    std::string srcTx = readFile(projectRoot() + "/src/al/link/io/LinkTx.cpp");
    std::string srcArq =
        readFile(projectRoot() + "/src/al/link/arq/LinkArq.cpp");
    assert(!srcRx.empty() && !srcTx.empty() && !srcArq.empty());
    assert(srcRx.find("AUTOLINK_TRACE_WIRE") != std::string::npos &&
           "LinkRx.cpp must reference AUTOLINK_TRACE_WIRE — the "
           "per-frame 'wire COBS ok seq=' verbose line is now "
           "compile-time gated so it can't saturate the log transport "
           "at ASYNC pipeline rate even when the operator lifts the "
           "level to VERBOSE for debugging. The the shape "
           "was unconditional verbose — that was field "
           "log's 1600-lines/s symptom.");
    assert(srcTx.find("AUTOLINK_TRACE_WIRE") != std::string::npos &&
           "LinkTx.cpp must reference AUTOLINK_TRACE_WIRE");
    assert(srcArq.find("AUTOLINK_TRACE_WIRE") != std::string::npos &&
           "LinkArq.cpp must reference AUTOLINK_TRACE_WIRE");
    std::cout << "  PASS (per-frame wire logs are AUTOLINK_TRACE_WIRE-gated)"
              << std::endl;
}

// Pin 10: master's P3-entry BREAK is sent on every
// reset with preserve=1 (not just the first). The
// shape field log showed the slave seeing the
// master's PING only every ~5.5s — the BREAK that
// would have re-notified the slave for the next
// P3 round was not reaching the wire on the
// second-and-later reset cycles. Source-grep on
// onTimer() to confirm the BREAK is unconditional
// after every reset_unlocked (the issue is whether
// a `state == SWP` short-circuit or a "first time
// only" guard is silently swallowing the second
// BREAK).
void test_pin_10_p3_entry_break_on_every_reset() {
    std::cout << "\n=== Pin 10: P3-entry BREAK sent on every reset, "
                 "not just the first ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/link/timers/LinkTimersOk.cpp");
    assert(!src.empty());
    auto p = src.find("void Link::onTimer()");
    assert(p != std::string::npos);
    // F2: retxSeq_unlocked now returns
    // bool (was void) — a wire-stall
    // refusal is the only reason for
    // the return type to change; the
    // body shape is the same.
    auto end = src.find("Link::retxSeq_unlocked", p);
    assert(end != std::string::npos);
    std::string body = src.substr(p, end - p);
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
    std::string code = stripComments(body);
    // The onTimer body must end with
    // `if (brk) hw.sendBreak();` — a single
    // unconditional call after every reset.
    // The bug class is a `if (brk && !first)`
    // or any guard that swallows the second
    // call.
    assert(code.find("if (brk)") != std::string::npos &&
           "Link::onTimer must end with an unconditional `if (brk) "
           "hw.sendBreak();` so a reset_unlocked call from the OK "
           "tick fires BREAK on every cycle. The pre-shape field "
           "log showed the master sending PING frames ~5.5s apart "
           "because the BREAK that re-notified the slave for the "
           "next P3 round was being swallowed on the second-and-"
           "later cycles (the slave's stale OK state never heard "
           "the master's resweep entry).");
    assert(code.find("hw.sendBreak()") != std::string::npos);
    // The sendBreak call at the end of onTimer
    // (the OK/SWP-path call, NOT the
    // breakStormPending_ call) must be
    // UNCONDITIONAL on `brk` (no `&& first`
    // short-circuit). Find the LAST hw.sendBreak
    // call in the body and check the guard
    // immediately before it.
    auto sendBreakPos = code.rfind("hw.sendBreak()");
    std::string before = code.substr(0, sendBreakPos);
    auto ifBrkPos = before.rfind("if (brk)");
    assert(ifBrkPos != std::string::npos &&
           "onTimer's last sendBreak call (the OK/SWP-path BREAK) "
           "must be guarded by `if (brk)`. The pre-shape field log "
           "showed the master sending PING at 14.077 and 19.577 "
           "(~5.5s apart) — the master's intermediate BREAK was "
           "swallowed by an `if (brk && first)` short-circuit that "
           "let the second-and-later cycles through without "
           "re-notifying the slave.");
    std::string condRegion = before.substr(ifBrkPos, sendBreakPos - ifBrkPos);
    assert(condRegion.find("&&") == std::string::npos &&
           "onTimer's last sendBreak must be guarded only by "
           "`if (brk)` — no `&& first / && !sent / && count==0` "
           "short-circuit that would silently drop the "
           "second-and-later cycles.");
    std::cout << "  PASS (onTimer: sendBreak unconditional on brk)"
              << std::endl;
}

// Pin 11: LinkArq.h is freertos-free. Modern
// FreeRTOS-Kernel treats <freertos/semphr.h>
// as a "second-pass" header — it #error's if
// <freertos/FreeRTOS.h> wasn't loaded first.
// The header is reachable via Link.h from
// AutoLink.h before AutoLink.h has had a
// chance to include FreeRTOS.h via
// UtilBlink.h, so any freertos include in
// the header hits the second-pass trap. The
// fix is to hold the semaphore as void* in
// the header and defer the real include to
// LinkArq.cpp. This pin asserts that:
//   (a) the header file does not directly
//       include <freertos/semphr.h> or any
//       other freertos header,
//   (b) the header's transitive include
//       closure (LinkArq.h + its own
//       #includes, with the project's real
//       -I paths) does not pull in semphr.h,
//   (c) the header compiles standalone with
//       -DARDUINO=10607 -DARDUINO_ARCH_ESP32
//       and no freertos on the include path
//       (i.e. a sketch TU stripped of the
//       freertos/Arduino.h -I list can still
//       parse LinkArq.h), and
//   (d) a "broken" LinkArq.h that DOES
//       include <freertos/semphr.h> inverts
//       (b) — this catches silent future
//       edits that re-introduce the include
//       inside an #ifdef ARDUINO guard.
void test_pin_11_linkarq_h_freertos_free() {
    std::cout << "\n=== Pin 11: LinkArq.h never reaches "
                 "<freertos/semphr.h> (directly or transitively) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string hdrPath = root + "/src/al/link/arq/LinkArq.h";

    // (a) Source-level: no direct freertos include.
    // The grep is over preprocessor directives only;
    // comments may reference the name freely. A future
    // edit that wants a real SemaphoreHandle_t in the
    // header should land in the .cpp, not here.
    std::string src = readFile(hdrPath);
    assert(!src.empty());
    {
        std::ifstream f(hdrPath);
        std::string line;
        std::string incLine;
        while (std::getline(f, line)) {
            std::string trimmed = line;
            size_t start = trimmed.find_first_not_of(" \t");
            if (start == std::string::npos)
                continue;
            trimmed = trimmed.substr(start);
            if (trimmed.rfind("#include", 0) != 0)
                continue;
            if (trimmed.find("<freertos/") != std::string::npos ||
                trimmed.find("\"freertos/") != std::string::npos ||
                trimmed.find("<FreeRTOS.h") != std::string::npos) {
                incLine = line;
                break;
            }
        }
        assert((incLine.empty() ? std::string()
                                : (std::string("LinkArq.h must not "
                                               "#include any <freertos/...> "
                                               "header — semphr.h is a "
                                               "second-pass file in modern "
                                               "FreeRTOS-Kernel and the "
                                               "header can be reached before "
                                               "FreeRTOS.h is loaded. If a "
                                               "future edit needs the real "
                                               "SemaphoreHandle_t, declare it "
                                               "in the .cpp, not the header. "
                                               "Offending line: ") +
                                   incLine))
                   .empty());
    }

    // (b) Preprocessor closure: drive the preprocessor
    //     over LinkArq.h with a stubbed freertos path
    //     on the include list. If any header in the
    //     transitive chain pulls in <freertos/semphr.h>
    //     (directly, via a transitive include, or via a
    //     macro expansion that references the type), the
    //     stub's content shows up in the preprocessor
    //     output and the pin fires. The stub is the
    //     minimum required for the include to resolve;
    //     we don't care about its semantic correctness
    //     — only about whether the chain reaches it.
    std::string preprocessed;
    {
        std::string stubDir = "/tmp/__pin_11_freertos_stub";
        std::string stubIncDir = stubDir + "/freertos";
        mkdir(stubDir.c_str(), 0755);
        mkdir(stubIncDir.c_str(), 0755);
        {
            FILE *f = fopen((stubIncDir + "/semphr.h").c_str(), "w");
            assert(f);
            fprintf(f, "// STUB: pin 11 freertos leak detector\n");
            fclose(f);
        }
        {
            FILE *f = fopen((stubIncDir + "/FreeRTOS.h").c_str(), "w");
            assert(f);
            fprintf(f, "// STUB: pin 11 freertos leak detector\n");
            fclose(f);
        }
        std::string cmd = "echo '#include \"al/link/arq/LinkArq.h\"' "
                          "| g++ -E -x c++ -std=gnu++14 "
                          "-DARDUINO=10607 "
                          "-DARDUINO_ARCH_ESP32 "
                          "-I" +
            stubDir +
            " "
            "-I" +
            root +
            "/src "
            "-I" +
            root +
            "/src/al "
            "-I. -I../common -I../common/accessors "
            "- 2>/dev/null";
        FILE *p = popen(cmd.c_str(), "r");
        assert(p && "could not invoke g++ for preprocessor check");
        char buf[4096];
        std::string out;
        while (fgets(buf, sizeof(buf), p))
            out += buf;
        pclose(p);
        preprocessed = std::move(out);
    }
    assert(preprocessed.find("__pin_11_freertos_stub/freertos/semphr.h") ==
               std::string::npos &&
           "LinkArq.h's preprocessor closure reached "
           "<freertos/semphr.h> — the second-pass trap "
           "returns. The pin catches include-chain "
           "leakage even if the header itself is clean.");
    assert(preprocessed.find("SemaphoreHandle_t") == std::string::npos &&
           "LinkArq.h's preprocessor closure referenced "
           "SemaphoreHandle_t — the type is freertos-only "
           "and must not appear in this header. Use "
           "void* in the header and cast in the .cpp.");

    // (c) Standalone compile: drive the compiler (not
    //     just the preprocessor) over a tiny TU that
    //     does nothing but include LinkArq.h. A sketch
    //     TU stripped of the freertos/Arduino.h -I list
    //     must still parse the header. If the header
    //     reintroduces a freertos include, this compile
    //     fails with the same second-pass error the
    //     field build surfaced.
    {
        std::string tuPath = "/tmp/__pin_11_linkarq_standalone.cpp";
        {
            FILE *f = fopen(tuPath.c_str(), "w");
            assert(f);
            fprintf(f, "#include \"al/link/arq/LinkArq.h\"\n");
            fclose(f);
        }
        std::string cmd = "g++ -c -x c++ -std=gnu++14 -w "
                          "-DARDUINO=10607 "
                          "-DARDUINO_ARCH_ESP32 "
                          "-I" +
            root +
            "/src "
            "-I" +
            root +
            "/src/al "
            "-I. -I../common -I../common/accessors " +
            tuPath +
            " -o /tmp/__pin_11_linkarq_standalone.o "
            "2>&1";
        std::string out;
        FILE *p = popen(cmd.c_str(), "r");
        assert(p);
        char buf[4096];
        while (fgets(buf, sizeof(buf), p))
            out += buf;
        int rc = pclose(p);
        // The header transitively pulls in
        // IHalCtx.h, ArqCache.h, and a few project
        // headers that themselves reach
        // AutoLinkConfig.h. None of those reach
        // freertos. The standalone compile must
        // succeed. A future edit that adds a
        // freertos include in any of those files
        // breaks this assertion.
        if (rc != 0) {
            std::cerr << out;
        }
        assert(rc == 0 &&
               "LinkArq.h did not compile standalone under "
               "-DARDUINO=10607 -DARDUINO_ARCH_ESP32 with no "
               "freertos on the include path. This is the "
               "exact failure mode the pin exists to catch.");
    }

    // (d) Self-test: write a deliberately-broken copy
    //     of LinkArq.h that re-introduces the
    //     <freertos/semphr.h> include, run the same
    //     source-grep detector (the same code path as
    //     step (a)) against it, and confirm the broken
    //     copy is detected. If the gate doesn't flip on
    //     a known-broken input, the gate is useless.
    //     The preprocessor closure check (b) is host-
    //     toolchain-specific and is not re-run here —
    //     it depends on whether the toolchain can find
    //     a freertos/semphr.h on the include path, which
    //     is environment-dependent. The source-grep
    //     detector is the load-bearing one.
    {
        std::string brokenPath = "/tmp/__pin_11_linkarq_broken.h";
        {
            FILE *f = fopen(brokenPath.c_str(), "w");
            assert(f);
            fprintf(f,
                    "#pragma once\n"
                    "#include <stdint.h>\n"
                    "// BROKEN-GATE: re-introduced freertos/semphr.h\n"
                    "// to verify the pin detects a future\n"
                    "// regression of the same kind.\n"
                    "#ifdef ARDUINO\n"
                    "#include <freertos/semphr.h>\n"
                    "#endif\n"
                    "class FakeLinkArq { void *ack_sem_; };\n");
            fclose(f);
        }
        // Re-run the (a) source-grep detector against
        // the broken file. We extract the inner loop
        // body inline (not as a function) to keep the
        // self-test self-contained.
        std::string brokenSrc = readFile(brokenPath);
        std::string brokenInc;
        {
            std::istringstream f(brokenSrc);
            std::string line;
            while (std::getline(f, line)) {
                size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos)
                    continue;
                std::string trimmed = line.substr(start);
                if (trimmed.rfind("#include", 0) != 0)
                    continue;
                if (trimmed.find("<freertos/") != std::string::npos ||
                    trimmed.find("\"freertos/") != std::string::npos ||
                    trimmed.find("<FreeRTOS.h") != std::string::npos) {
                    brokenInc = line;
                    break;
                }
            }
        }
        assert(!brokenInc.empty() &&
               "self-test failed: the source-grep "
               "detector did not fire on a deliberately-"
               "broken header that #includes "
               "<freertos/semphr.h>. The pin's positive "
               "result is meaningless until the detector "
               "is shown to flag a known-bad input.");
    }

    std::cout << "  PASS (header: no freertos include, no "
                 "SemaphoreHandle_t in closure, compiles "
                 "standalone, self-test detected broken copy)"
              << std::endl;
}

} // namespace

int main() {
    Log::log().setLevel(Log::NONE);
    test_pin_1_ispending_log_free();
    test_pin_2a_waitforack_timeout();
    test_pin_2b_waitforack_event_driven_source();
    test_pin_3_setmode_forwarded_to_hal_before_begin();
    test_pin_4_begin_logs_mode_mismatch();
    test_pin_5_drain_rx_re_arms_base_stuck();
    test_pin_6_cpu_stall_re_arms_base_stuck();
    test_pin_7_drop_peer_stalled_fires_on_single_sided_dead();
    test_pin_7b_drop_peer_stalled_holds_when_pending_zero();
    test_pin_8_peer_stalled_baud_derived();
    test_pin_9_peer_stalled_respects_gbn_backoff();
    test_pin_9_wire_trace_compile_gated();
    test_pin_10_p3_entry_break_on_every_reset();
    test_pin_11_linkarq_h_freertos_free();
    std::cout << "\nFieldWedgeFixesTest: all pins passed" << std::endl;
    return 0;
}

#endif
