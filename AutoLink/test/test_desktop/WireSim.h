// WireSim.h — in-process two-node AutoLink simulator (v5.1.37+).
//
// The host test suite historically verified ALink in isolation
// (loopback_test.cpp) and the AutoLink facade in isolation
// (AutoLinkFacadeTest.cpp). The two never met: the facade tests
// bypassed the protocol with test_arqCache_put hooks, and the
// loopback test bypassed the facade by using raw ALink. Bugs in
// the FACADE's interaction with the protocol across link drops
// (the v5.1.35-36 series) slipped through this gap.
//
// WireSim fixes that by promoting MockHal into a first-class wire
// model that connects two full AutoLink instances back-to-back in
// one host process. The test then drives the real UtilPing/UtilPong
// loop bodies — or their host-side equivalent — against the wire.
//
// Usage:
//   WireSim sim;
//   sim.setFrameDropPct(2);             // 2% frame loss
//   sim.setForcedDropEvery(800);        // drop the link every 800 cycles
//   TwoNodeFixture f(sim);
//   f.begin();                          // negotiate to OK
//   for (int i = 0; i < 5000; i++) {
//       f.step(1);                      // 1ms of simulated time
//       ASSERT(f.bytesTransferredAtoB() > prev);
//       if (i % 800 == 799) {
//           ASSERT(f.pendingCountA() == 0);  // gate didn't latch
//       }
//   }
//
// What the test observes:
//   - bytesTransferred: cumulative TX bytes from A to B (and vice versa)
//   - pendingCount:     facade ARQ cache occupancy on each side
//   - dropsInjected:    count of forced drops applied
//   - framesDropped:    count of wire-noise frames dropped
//   - getState:         SWP / LCK / OK on each side
//
// The whole point: every bug found in v5.1.36 would have failed the
// closed-loop test on the first run. The reason they didn't is that
// the host suite tested ALink and the facade separately, never the
// closed loop through both across a re-sweep.

#pragma once
#ifndef AUTOLINK_HOST_TEST
#error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "MockHal.h"
#include "AutoLink.h"

#include <cstdint>
#include <vector>
#include <cassert>
#include <iostream>

namespace autolink {

class WireSim {
public:
    WireSim(AutoLinkConfig cfg = AutoLinkConfig())
        : mA_(new MockHal())
        , mB_(new MockHal())
        , a_(mA_.get(), /*isMasterNode=*/true, cfg)    // master / Ping
        , b_(mB_.get(), /*isMasterNode=*/false, cfg)   // slave  / Pong
    {
        // Wire the two MockHals together: sendBreak on A delivers
        // onBreak to B (mirrors real wire semantics where BREAK
        // appears on the peer's RX pin asynchronously).
        mA_->peer = mB_.get();
        mB_->peer = mA_.get();
        // Share a clock so both sides see the same nowMs().
        mA_->now = 0;
        mB_->now = 0;
    }

    // ---- knobs (call BEFORE step()) -----------------------------

    // Frame drop percentage in [0, 100]. Each frame (delimited by
    // the COBS 0x00 sentinel) has this chance of being dropped
    // before being delivered. Deterministic via the LCG seed so
    // test failures are reproducible.
    void setFrameDropPct(int pct) {
        mA_->frameDropPct = pct;
        mB_->frameDropPct = pct;
    }

    // Force a link drop every N steps (test can also call forceDrop()
    // directly). 0 disables.
    void setForcedDropEvery(int n) { forcedDropEvery_ = n; }

    // Force a drop on the next step. Useful for tests that want
    // specific drop points.
    void requestDrop() { dropPending_ = true; }

    // ---- accessors -----------------------------------------------

    MockHal& halA() { return *mA_; }
    MockHal& halB() { return *mB_; }
    AutoLink& linkA() { return a_; }   // Ping
    AutoLink& linkB() { return b_; }   // Pong

    // ---- pumps ----------------------------------------------------

    // Advance both clocks by `deltaMs`, then run one tick on each
    // side and pipe the resulting bytes across the wire (with
    // frame-drop). Also handles forced drops.
    // v5.1.40 (injectable clock): use MockHal's deterministic
    // pumpClock() instead of manual onTimer() calls. pumpClock
    // advances time AND fires onTimer() only when the deadline
    // has elapsed — making idle-timeout, ACK-timeout, and
    // sweep-stall tests sub-ms deterministic without real wall-
    // clock waits. Loop until no timer is due (a fired timer
    // may re-arm, e.g. OK->SWP transition restarts the sweep
    // timer).
    void step(uint32_t deltaMs) {
        // Snapshot protocol drops BEFORE the step so we can detect
        // drops the protocol layer triggers internally (err_unlocked
        // path, cache-miss path, etc.). The protocol's dropLink()
        // bumps discCount; the diff is the number of protocol-
        // driven drops that fired this tick.
        Stats preA, preB;
        a_.linkForTest()->getStats(preA);
        b_.linkForTest()->getStats(preB);
        // Forced drop (if any). The drop itself is on the link
        // level (the protocol layer); we model that by calling
        // linkA->dropLink() which triggers reset_unlocked ->
        // linkResetCallback_ -> arqCache_clearAll. This is exactly
        // the production path.
        if (dropPending_ || (forcedDropEvery_ > 0 && (++stepCount_) % forcedDropEvery_ == 0)) {
            if (dropPending_ || (forcedDropEvery_ > 0)) {
                // Only the master (Ping) initiates a drop, mirroring
                // real wire behavior where the master detects first.
                a_.dropLink();
                dropsInjected_++;
                dropPending_ = false;
            }
        }
        // v5.1.40: pumpClock advances BOTH clocks by deltaMs AND
        // fires onTimer() deterministically when the protocol's
        // scheduled deadline has elapsed. Replaces the v5.1.38
        // manual onTimer() calls (which fired every step regardless
        // of timing — so ACK_RTO_MS, idle-timeout, and sweep-stall
        // behavior was untestable as written). Bound the inner
        // loop to avoid pathological re-arm chains.
        int fireCountA = 0, fireCountB = 0;
        mA_->now += deltaMs;
        mB_->now += deltaMs;
        while ((mA_->nextTimerAtMs != UINT32_MAX && mA_->now >= mA_->nextTimerAtMs) && fireCountA++ < 32) {
            mA_->timerFiredCalls++;
            a_.linkForTest()->onTimer();
        }
        while ((mB_->nextTimerAtMs != UINT32_MAX && mB_->now >= mB_->nextTimerAtMs) && fireCountB++ < 32) {
            mB_->timerFiredCalls++;
            b_.linkForTest()->onTimer();
        }
        // Pipe bytes across the wire. pipe_data() applies frame drop
        // per side.
        pipe_data(*mA_, *mB_);
        pipe_data(*mB_, *mA_);
        // Detect protocol-driven drops.
        Stats postA, postB;
        a_.linkForTest()->getStats(postA);
        b_.linkForTest()->getStats(postB);
        int protoDropsThisStep =
            (int)((postA.discCount - preA.discCount) +
                  (postB.discCount - preB.discCount));
        if (protoDropsThisStep > 0) protoDropsSeen_ += protoDropsThisStep;
    }

    // ---- counters / state ---------------------------------------

    // Total wire bytes that crossed A->B (NOT counting drops).
    // Both sides' TX bytes count: A's txBuf is the source, B's
    // linkForTest() accumulator is the sink.
    uint64_t bytesTransferredAtoB() const {
        // Sum from both sides' facade-level tx bytes (counts payload)
        // and the raw wire bytes that crossed.
        Stats sa, sb; a_.linkForTest()->getStats(sa); b_.linkForTest()->getStats(sb);
        // tx counts bytes ACCEPTED to the HAL (B's perspective). Use
        // b_.linkForTest()'s rx (bytes received by B from A).
        return sb.rx;
    }
    uint64_t bytesTransferredBtoA() const {
        Stats sa, sb; a_.linkForTest()->getStats(sa); b_.linkForTest()->getStats(sb);
        return sa.rx;
    }

    int dropsInjected()  const { return dropsInjected_; }
    int protoDropsSeen() const { return protoDropsSeen_; }
    int framesDropped()  const { return mA_->bytesDropped + mB_->bytesDropped; }
    int pendingCountA()  const { return a_.arqCacheSizeForTest(); }
    int pendingCountB()  const { return b_.arqCacheSizeForTest(); }
    State getStateA()    const { return a_.getState(); }
    State getStateB()    const { return b_.getState(); }
    // v5.1.40: test accessors for the underlying AutoLink and
    // MockHal instances. The MockHal pointers are needed for
    // pumpClock (deterministic clock pump). The AutoLink refs
    // are needed for setLinkPaused, getStats, dropLink etc.
    AutoLink& nodeAForTest() { return a_; }
    AutoLink& nodeBForTest() { return b_; }
    // Test accessors (read-only, host-only). The test wants to
    // inspect the raw txBuf size to diagnose "bytes stuck"
    // failures. Kept simple — return by const ref, no copies.
    const MockHal& rawA() const { return *mA_; }
    const MockHal& rawB() const { return *mB_; }

private:
    // AutoLinks must be declared BEFORE the MockHals so that
    // during destruction the AutoLinks run first (their unique_ptr
    // deletes the MockHal), then the unique_ptr<MockHal> dtors
    // run on already-freed pointers. Wait — that's still use-after-free.
    //
    // The right fix: don't let the AutoLink own the ILink. Use a
    // non-owning observer. Since C++ doesn't have observer pointers
    // in unique_ptr, we use a raw `ILink*` member in AutoLink for
    // the host-only path. But that requires changing the hal
    // member to a raw pointer for host. Simplest workaround: use
    // a custom no-op deleter.
    //
    // Actually the cleanest solution is: AutoLink doesn't own the
    // ILink when the ILink* constructor is used. The ILink* is
    // borrowed; the caller (WireSim) owns it. WireSim's
    // destruction order is reverse-of-declaration, so by declaring
    // the AutoLinks AFTER the MockHals, the AutoLinks are
    // destroyed first. Their dtor's unique_ptr<ILink> default-
    // deletes the ILink — but the caller still owns it, so
    // WireSim then tries to free the same pointer via its own
    // unique_ptr. That's the double-free.
    //
    // Solution: use a no-op deleter on the AutoLink's ILink when
    // the ILink* is borrowed. WireSim's unique_ptr<MockHal> then
    // does the actual delete.
    std::unique_ptr<MockHal> mA_;
    std::unique_ptr<MockHal> mB_;
    AutoLink  a_;   // master (Ping)
    AutoLink  b_;   // slave  (Pong)

    int  forcedDropEvery_ = 0;
    int  stepCount_        = 0;
    bool dropPending_      = false;
    int  dropsInjected_    = 0;
    // Detect protocol-driven drops (link layer's own dropLink
    // path, called from err_unlocked / cache-miss). The protocol
    // doesn't notify us; we poll the stats counters before and
    // after each step and bump this counter on the diff.
    uint64_t lastDiscA_     = 0;
    uint64_t lastDiscB_     = 0;
    int      protoDropsSeen_ = 0;
};

// TwoNodeFixture is the convenience wrapper. It owns a WireSim
// and exposes a `step()` that:
//   1. calls WireSim::step(deltaMs) (clock + onTimer + wire)
//   2. drives the Ping loop body (send until window full)
//   3. drives the Pong loop body (recv -> echo)
//   4. drives the Ping loop body's recv (drain echoes)
//
// All of (2)(3)(4) is the same loop pattern as UtilPing/UtilPong
// inlined for the host (no Arduino dependencies: no millis(), no
// random(), no log_).
class TwoNodeFixture {
public:
    explicit TwoNodeFixture(WireSim& sim) : sim_(sim) {
        pendingA_.assign(32, Slot{false, 0, 0, 0});
        pendingB_.assign(32, Slot{false, 0, 0, 0});
    }
    // v5.1.40: expose the AutoLink instances so clock-injection
    // tests can call setLinkPaused, getStats, etc.
    AutoLink& nodeA() { return sim_.nodeAForTest(); }
    AutoLink& nodeB() { return sim_.nodeBForTest(); }

    // Kick off the protocol. Must be called before step(). The
    // first few steps will negotiate SWP -> LCK -> OK.
    void begin() {
        sim_.linkA().begin();
        sim_.linkB().begin();
    }

    // One full closed-loop tick. Returns the number of bytes that
    // crossed the wire in this tick.
    uint64_t step(uint32_t deltaMs) {
        // If the link was just dropped (forced OR protocol-driven),
        // the test-side in-flight tracking (pendingA_/pendingB_)
        // is meaningless — the protocol's send buffers are reset
        // on dropLink, and the pending messages will never come
        // back. Clear our local tracking so the next drivePing_
        // can send again. This mirrors what the production
        // UtilPing loop body does on a drop (returns to the
        // "ready to send" state).
        int dropsBefore = sim_.dropsInjected() + sim_.protoDropsSeen();
        sim_.step(deltaMs);
        int dropsAfter = sim_.dropsInjected() + sim_.protoDropsSeen();
        if (dropsAfter > dropsBefore) {
            for (auto& s : pendingA_) s.active = false;
            for (auto& s : pendingB_) s.active = false;
        }
        // Drive the Ping loop body: send until window full or
        // MAX_TX_PER_LOOP reached, drain any echoed messages.
        drivePing_();
        // Drive the Pong loop body: recv -> echo.
        drivePong_();
        return sim_.bytesTransferredAtoB();
    }

    // Convenience: total bytes A->B since start.
    uint64_t totalBytesAtoB() const { return sim_.bytesTransferredAtoB(); }
    // Test accessors: raw underlying WireSim and MockHals. Used by
    // diagnostic output in failure paths.
    WireSim&      sim()      { return sim_; }
    const MockHal& halA()    { return sim_.rawA(); }
    const MockHal& halB()    { return sim_.rawB(); }
    int pendingCountA() { sim_.halA().now;  return sim_.pendingCountA(); }
    int pendingCountB() { sim_.halB().now;  return sim_.pendingCountB(); }
    State getStateA()   const { return sim_.getStateA(); }
    State getStateB()   const { return sim_.getStateB(); }

    // Tunables for the host test (override defaults after ctor).
    int maxBurstPerLoop = 4;     // lower than UtilPing's 16 — host
                                 // loops are tighter (no FreeRTOS
                                 // scheduling). Closed loop converges
                                 // faster with smaller bursts.
    int msgSize         = 64;    // payload bytes per sendMsg
    int windowSize      = 32;    // matches UtilPing::WINDOW
    bool debug_log_     = false; // set true for per-step diagnostics

private:
    struct Slot { bool active; int len; uint16_t crc; uint32_t sentMs; };
    std::vector<Slot> pendingA_;
    std::vector<Slot> pendingB_;

    WireSim& sim_;
    uint32_t seqSeed_ = 0x12345;

    uint8_t nextByte_() {
        seqSeed_ = seqSeed_ * 1103515245u + 12345u;
        return (uint8_t)(seqSeed_ >> 16);
    }

    void fillPayload_(uint8_t* buf, int n) {
        for (int i = 0; i < n; i++) buf[i] = nextByte_();
    }

    // UtilPing loop body (host-side): fill WINDOW with new sends
    // up to MAX_TX_PER_LOOP, drain echoes into pendingA_ slots.
    void drivePing_() {
        if (sim_.getStateA() != State::OK) return;
        // Drain echoes (bytes the Pong side sent back).
        uint8_t buf[1024];
        int n;
        while ((n = sim_.linkA().recvMsg(buf, sizeof buf)) > 0) {
            // Mark the matching pendingA_ slot as completed.
            for (auto& s : pendingA_) {
                if (s.active && s.len == n) {
                    s.active = false;
                    break;
                }
            }
        }
        // Send up to maxBurstPerLoop new messages, but only if
        // we have free slots in our app-level window.
        int sent = 0;
        for (auto& s : pendingA_) {
            if (sent >= maxBurstPerLoop) break;
            if (s.active) continue;
            int len = msgSize;
            fillPayload_(buf, len);
            bool ok = sim_.linkA().sendMsg(buf, len);
            if (debug_log_) {
                std::cerr << "[drivePing_] sendMsg(" << len << ") -> " << (ok?"true":"false")
                          << " stateA=" << (int)sim_.getStateA()
                          << " facadeCache=" << sim_.pendingCountA() << std::endl;
            }
            if (ok) {
                s.active = true;
                s.len    = len;
                s.crc    = 0;  // CRC not used by the test
                s.sentMs = sim_.halA().now;
                sent++;
            } else {
                // sendMsg refused (cache full, link not OK, etc.)
                // — try again next loop.
                break;
            }
        }
        if (debug_log_) {
            std::cerr << "[drivePing_] sent=" << sent
                      << " pendingA_=" << pendingA_.size() << std::endl;
        }
    }

    // UtilPong loop body (host-side): recv -> echo.
    void drivePong_() {
        if (sim_.getStateB() != State::OK) return;
        uint8_t buf[1024];
        int n;
        int sent = 0;
        while ((n = sim_.linkB().recvMsg(buf, sizeof buf)) > 0 && sent < maxBurstPerLoop) {
            if (sim_.linkB().sendMsg(buf, n)) {
                sent++;
            }
        }
    }
};

} // namespace autolink
