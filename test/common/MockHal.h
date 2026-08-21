// Host IHal: memory-pipe TX/RX,
// injectable clock, frame-drop model.
#pragma once
#ifndef ARDUINO

#    include "al/link/Link.h"
#    include <iostream>
#    include <queue>
#    include <mutex>
#    include <vector>
#    include <cstdint>
#    include <cassert>
#    include <functional>

namespace autolink {
class MockHal : public IHal {
public:
    uint32_t spd = 9600;
    AutoLinkConfig::Mode mode = AutoLinkConfig::Mode::SYNC;
    bool timerActive = false;
    std::vector<uint8_t> txBuf;

    std::vector<uint32_t> txBaudPerByte;

    uint32_t txBaud = 9600;

    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    int timerFiredCalls = 0;
    int lockDepth = 0;
    std::function<void()> onSetSpd;
    std::vector<uint32_t> spdHistory;

    uint32_t nextTimerAtMs = UINT32_MAX;

    MockHal *peer = nullptr;

    int frameDropPct = 0;
    // Finer-grained drop rate for the loss-sweep
    // itest (0.1% is not representable in whole
    // percent). Wins over frameDropPct when > 0.
    int frameDropPermille = 0;
    // True per-frame loss: drops whole 0x00-delimited
    // COBS frames instead of 5-byte blocks. Block
    // drops corrupt large frames far above the
    // nominal rate (a 250 B frame is ~50 blocks);
    // this knob makes "1% frame loss" mean 1% of
    // frames. Wins over both block knobs when > 0.
    int wholeFrameDropPermille = 0;
    int bytesDropped = 0;
    uint32_t dropRngSeed = 1;
    int dropNextFrames = 0;

    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx;

    void begin() override {}
    // J3 + J4: config-aware
    // begin. The IHal
    // virtual's default
    // begin() is a no-op;
    // the override calls
    // setupForCfg to size
    // the ring from the
    // config. The
    // txCapUserSet_ flag
    // tracks whether the
    // test has explicitly
    // set txCap; only if
    // it hasn't do we
    // derive from config.
    // Tests that pre-set
    // txCap (e.g.
    // TxRingStallReasonTest,
    // which sizes the ring
    // to exactly one chunk
    // + 1 byte) keep
    // their override; the
    // 29 binaries that
    // never touch txCap
    // get a realistic
    // 2-3x-floor ring by
    // default.
    bool txCapUserSet_ = false;
    void begin(const AutoLinkConfig &cfg) override {
        if (txCapUserSet_)
            return;
        setupForCfg(cfg);
        txCapUserSet_ = true;
    }
    // I1: report the installed
    // ring size to Link::begin
    // for the post-cap gate.
    // 0 = "no floor" (the
    // historical non-binding
    // cap; the gate treats 0
    // as "skip"). Pinned by
    // BeginRejectsHeapClampedRingTest.
    size_t txRingSize() const override { return txCap; }
    void setMode(AutoLinkConfig::Mode m) override { mode = m; }
    AutoLinkConfig::Mode getMode() const override { return mode; }
    void setSpd(uint32_t s) override {
        spd = s;
        spdHistory.push_back(s);
        if (onSetSpd)
            onSetSpd();
    }
    void sendBreak() override {
        sendBreakCalls++;
        if (peer && peer->events())
            peer->events()->onBreak();
    }
    void deliver_break_to_self() {
        if (events())
            events()->onBreak();
    }
    // Non-binding by default so a test that does not care about ring
    // size is unaffected; setupForCfg(cfg) drops it to the production
    // floor. Pinned by MockHalRingMatchesFloorTest.
    size_t txCap = 65536;
    // J3: tests that
    // explicitly size
    // txCap (e.g. the
    // ring-too-small
    // regression tests)
    // set this flag so
    // begin(cfg) doesn't
    // override their
    // value with the
    // config floor.
    void setTxCapForTest(size_t cap) {
        txCap = cap;
        txCapUserSet_ = true;
    }
    void setupForCfg(const AutoLinkConfig &cfg) {
        size_t want = uartTxBufferFloor(cfg);
        // No extra floor — the
        // production HAL applies
        // exactly uartTxBufferFloor
        // and the test HAL should
        // mirror that. Tests that
        // need a larger cap (for
        // non-binding tests) set
        // txCap directly after
        // setupForCfg returns.
        txCap = want;
    }
    int txAvail() const override {
        int room = (int)txCap - (int)txBuf.size();
        return room < 0 ? 0 : room;
    }
    // Largest single tx() call accepted; lets a test bound the framer's
    // real worst-case frame against kWorstCaseCobsFrame.
    int maxTxCall = 0;
    int tx(const uint8_t *b, int n) override {
        if (n > maxTxCall)
            maxTxCall = n;
        int room = (int)txCap - (int)txBuf.size();
        if (room < 0)
            room = 0;
        if (n > room)
            n = room;
        if (txFailN > 0) {
            int take = n > txFailN ? n - txFailN : 0;
            if (take > 0) {
                for (int i = 0; i < take; i++) {
                    txBuf.push_back(b[i]);
                    txBaudPerByte.push_back(spd);
                }
            }
            txFailN = 0;
            return take;
        }
        for (int i = 0; i < n; i++) {
            txBuf.push_back(b[i]);
            txBaudPerByte.push_back(spd);
        }

        return n;
    }
    void flushTx() override {}
    int discardTxCalls = 0;
    void discardTx() override { discardTxCalls++; }
    void startTimer(int ms) override {
        timerStartCalls++;
        timerActive = true;
        lastTimerMs = ms;
        nextTimerAtMs = now + (uint32_t)ms;
    }
    void stopTimer() override {
        timerActive = false;
        nextTimerAtMs = UINT32_MAX;
    }

    void pumpClock(uint32_t deltaMs) {
        now += deltaMs;
        int safety = 0;
        // One-shot, like the real xTimerCreate(pdFALSE)
        // timer: a fire consumes the arm, and the tick
        // only repeats if the callee re-arms. The prior
        // shape left nextTimerAtMs stale and re-fired a
        // dead timer, hiding rearm bugs from the host
        // suite.
        while (nextTimerAtMs != UINT32_MAX && now >= nextTimerAtMs &&
               safety++ < 16) {
            timerFiredCalls++;
            nextTimerAtMs = UINT32_MAX;
            timerActive = false;
            if (events())
                events()->onTimer();
        }
    }

    void runFor(uint32_t targetMs) {
        uint32_t end = now + targetMs;
        while (now < end) {
            uint32_t chunk = end - now;
            if (chunk > 100)
                chunk = 100;
            pumpClock(chunk);
        }
    }
    void delayMs(int ms) override {
        if (ms > 0)
            now += (uint32_t)ms;
    }
    // Sub-ms pacing for the inter-chunk gap test. Advances
    // the mock clock by `us` microseconds (rounded up to ms
    // for pumpClock's whole-ms granularity). The
    // AsyncChunkGapTest asserts the *gap magnitude* on the
    // mock clock rather than a wall-clock measurement so the
    // host test stays subsecond.
    uint64_t totalDelayUs = 0;
    int delayUsCalls = 0;
    void delayUs(uint32_t us) override {
        delayUsCalls++;
        totalDelayUs += us;
        uint32_t ms = (us + 999) / 1000;
        if (ms > 0)
            now += ms;
    }
    void clearTx() {
        txBuf.clear();
        txBaudPerByte.clear();
    }

    // Threaded pumps must use this instead of an unlocked
    // copy + clearTx(): Link appends to txBuf under mtx, and
    // an unlocked copy+clear races the append, silently
    // dropping TX bytes mid-frame.
    std::vector<uint8_t> drainTx() {
        std::lock_guard<std::mutex> g(mtx);
        std::vector<uint8_t> out;
        out.swap(txBuf);
        txBaudPerByte.clear();
        return out;
    }

    uint32_t now = 0;
    int lastTimerMs = 0;
    uint32_t nowMs() override { return now; }

    size_t appBufCap = (size_t)-1;
    int txFailN = 0;
    // F1: when set, unlock() asserts if
    // called while inside an onRx/onTimer
    // call frame. Lets a test pin "no
    // lock drop on a path reachable from
    // onRx" — drive a retx burst from
    // inside a synthetic onRx, and any
    // path that drops the lock aborts
    // the test. Toggle off (drop the
    // assert) -> the F1 invariant is
    // silently lost.
    bool assertUnlockForbiddenInRx = false;
    bool inOnRxFrame = false;

    void lock() override {
        mtx.lock();
        lockDepth++;
    }
    void unlock() override {
        if (assertUnlockForbiddenInRx && inOnRxFrame) {
            std::cerr << "\nFAIL: lock dropped on a path reachable from "
                         "onRx — same defect class as D1, one layer down"
                      << std::endl;
            assert(false);
        }
        lockDepth--;
        mtx.unlock();
    }

    // tryLock instrumentation for EventTaskBoundedLockTest: records
    // every call's timeoutMs and, when forceTryLockFail is set,
    // returns false without touching mtx — models the event task
    // giving up rather than parking behind a contended lock. When
    // not forced to fail, behaves like lock() (host tests are
    // single-threaded, so there's no real contention to time out
    // on).
    bool forceTryLockFail = false;
    int tryLockCalls = 0;
    int lastTryLockTimeoutMs = -1;
    bool tryLock(int timeoutMs) override {
        tryLockCalls++;
        lastTryLockTimeoutMs = timeoutMs;
        if (forceTryLockFail)
            return false;
        mtx.lock();
        lockDepth++;
        return true;
    }

    int pushAppBuf(const uint8_t *b, int n) override {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            if (appBuf.size() >= appBufCap)
                break;
            appBuf.push(b[i]);
            acc++;
        }
        return acc;
    }
    int popAppBuf(uint8_t *b, int max_len) override {
        int n = 0;
        while (n < max_len && !appBuf.empty()) {
            b[n++] = appBuf.front();
            appBuf.pop();
        }
        return n;
    }
    int peekAppBuf() const override {
        if (appBuf.empty())
            return -1;
        return appBuf.front();
    }
    int peekAt(uint8_t *out, int n, int offset) const override {
        if (n <= 0 || offset < 0 || offset >= (int)appBuf.size())
            return 0;
        std::vector<uint8_t> tmp(appBuf.size());
        int i = 0;
        std::queue<uint8_t> copy = appBuf;
        while (!copy.empty()) {
            tmp[i++] = copy.front();
            copy.pop();
        }
        int copied = 0;
        for (int k = offset; k < (int)tmp.size() && copied < n; k++) {
            out[copied++] = tmp[k];
        }
        return copied;
    }
    int appBufAvailable() const override { return (int)appBuf.size(); }
    int appBufFree() const override {
        if (appBufCap == (size_t)-1)
            return INT32_MAX;
        return (int)(appBufCap - appBuf.size());
    }
    void clearAppBuf() override {
        while (!appBuf.empty())
            appBuf.pop();
    }
};

inline void pipe_data(MockHal &src, MockHal &dest) {
    if (src.txBuf.empty())
        return;

    std::vector<uint8_t> kept;
    kept.reserve(src.txBuf.size());
    for (size_t i = 0; i < src.txBuf.size(); i++) {
        if (src.txBaudPerByte[i] == dest.spd) {
            kept.push_back(src.txBuf[i]);
        } else {
            src.bytesDropped++;
        }
    }
    src.txBuf.clear();
    src.txBaudPerByte.clear();
    if (kept.empty())
        return;
    if (src.wholeFrameDropPermille > 0) {
        // Segment the stream into whole frames and
        // drop them at the given rate: 0x00 bytes
        // pass through (COBS delimiters), 0xAA 0x55
        // ctrl frames are atomic 5-byte units (they
        // can contain interior 0x00s — splitting one
        // injects garbage mid-stream), everything
        // else segments on the next 0x00.
        uint32_t s = src.dropRngSeed ? src.dropRngSeed : 1;
        std::vector<uint8_t> delivered;
        size_t i = 0;
        while (i < kept.size()) {
            if (kept[i] == 0x00) {
                delivered.push_back(kept[i++]);
                continue;
            }
            size_t j;
            if (kept[i] == 0xAA && i + 1 < kept.size() && kept[i + 1] == 0x55) {
                j = i + 5 <= kept.size() ? i + 5 : kept.size();
            } else {
                j = i;
                while (j < kept.size() && kept[j] != 0x00)
                    j++;
            }
            bool drop = ((s = s * 1664525u + 1013904223u) % 1000u) <
                (uint32_t)src.wholeFrameDropPermille;
            if (drop)
                src.bytesDropped += (int)(j - i);
            else
                delivered.insert(delivered.end(), kept.begin() + i,
                                 kept.begin() + j);
            i = j;
        }
        src.dropRngSeed = s;
        if (!delivered.empty() && dest.events())
            dest.events()->onRx(delivered.data(), delivered.size());
        return;
    }
    uint32_t dropPermille = src.frameDropPermille > 0
        ? (uint32_t)src.frameDropPermille
        : (src.frameDropPct > 0 ? (uint32_t)(src.frameDropPct * 10) : 0);
    if (dropPermille == 0) {
        if (dest.events())
            dest.events()->onRx(kept.data(), kept.size());
        return;
    }

    uint32_t s = src.dropRngSeed ? src.dropRngSeed : 1;
    std::vector<uint8_t> delivered;
    size_t i = 0;
    while (i + 5 <= kept.size()) {
        bool drop = ((s = s * 1664525u + 1013904223u) % 1000u) < dropPermille;
        if (!drop && src.dropNextFrames > 0) {
            drop = true;
            src.dropNextFrames--;
        }
        if (drop) {
            src.bytesDropped += 5;
        } else {
            delivered.insert(delivered.end(), kept.begin() + i,
                             kept.begin() + i + 5);
        }
        i += 5;
    }

    if (i < kept.size()) {
        if (src.dropNextFrames > 0) {
            src.bytesDropped += (int)(kept.size() - i);
            src.dropNextFrames--;
        } else {
            delivered.insert(delivered.end(), kept.begin() + i, kept.end());
        }
    }
    src.dropRngSeed = s;
    if (!delivered.empty() && dest.events())
        dest.events()->onRx(delivered.data(), delivered.size());
}

inline void negotiate_to_ok(Link &ping, Link &pong, MockHal &mHal,
                            MockHal &sHal) {
    ping.begin();
    pong.begin();
    int N = (int)ping.getConfig().allowedBaudsCount;

    sHal.setSpd(ping.getConfig().allowedBauds[N - 1]);

    for (int i = 0; i < 200; i++) {
        uint32_t targetMs = mHal.now + 50;
        while (mHal.now < targetMs) {
            mHal.pumpClock(targetMs - mHal.now);
            sHal.pumpClock(targetMs - sHal.now);
            pipe_data(mHal, sHal);
            pipe_data(sHal, mHal);
            if (ping.getState() == State::OK && pong.getState() == State::OK)
                break;
        }
        // Sweep locks the master at the fastest
        // baud. The slave is still parked at the
        // slowest baud; snap it to the master's
        // locked baud so the LOCK_CMD frame is
        // delivered. After this snap the slave
        // receives LOCK_CMD, enters OK, and both
        // sides converge at the master's baud.
        if (ping.getState() == State::OK && pong.getState() != State::OK) {
            sHal.setSpd(
                ping.getConfig().allowedBauds[ping.getCurrentSpdIndex()]);
            pipe_data(mHal, sHal);
        }
        if (ping.getState() == State::OK && pong.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    // Snap both HALs to the master's locked baud.
    // The in-loop sHal.setSpd fires only when the
    // slave is still in OK-wait after the master's
    // lock; the new P1->P2 promotion closes that
    // gap (the slave reaches OK in the same
    // iteration as the master's lock), so the
    // in-loop sync no longer fires. Sync here so
    // downstream sendMsg/pipe_data flows run on a
    // baud-matched wire.
    sHal.setSpd(ping.getConfig().allowedBauds[ping.getCurrentSpdIndex()]);
    // Pump 100ms of clock to close the
    // post-lock wire-settle gate
    // (AUTOLINK_WIRE_SETTLE_MS = 50ms).
    // The link-layer drops every incoming
    // frame for AUTOLINK_WIRE_SETTLE_MS after
    // a successful lock; downstream tests
    // that send a single message and check
    // `available()` immediately would lose
    // that message to the settle gate.
    // 100ms is 2x the gate, with margin for
    // the test's per-step 20ms clock tick.
    for (int i = 0; i < 5; i++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
}

} // namespace autolink

#endif