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

namespace autolink
{
class MockHal : public IHal
{
public:
    uint32_t spd = 9600;
    bool timerActive = false;
    std::vector<uint8_t> txBuf;

    std::vector<uint32_t> txBaudPerByte;

    uint32_t txBaud = 9600;

    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    int timerFiredCalls = 0;
    std::vector<uint32_t> spdHistory;

    uint32_t nextTimerAtMs = UINT32_MAX;

    MockHal *peer = nullptr;

    int frameDropPct = 0;
    int bytesDropped = 0;
    uint32_t dropRngSeed = 1;
    int dropNextFrames = 0;

    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx;

    void begin() override {}
    void setSpd(uint32_t s) override
    {
        spd = s;
        spdHistory.push_back(s);
    }
    void sendBreak() override
    {
        sendBreakCalls++;
        if (peer && peer->link)
            peer->link->onBreak();
    }
    void deliver_break_to_self()
    {
        if (link)
            link->onBreak();
    }
    int tx(const uint8_t *b, int n) override
    {
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
    void startTimer(int ms) override
    {
        timerStartCalls++;
        timerActive = true;
        lastTimerMs = ms;
        nextTimerAtMs = now + (uint32_t)ms;
    }
    void stopTimer() override
    {
        timerActive = false;
        nextTimerAtMs = UINT32_MAX;
    }

    void pumpClock(uint32_t deltaMs)
    {
        now += deltaMs;
        int safety = 0;
        while (nextTimerAtMs != UINT32_MAX && now >= nextTimerAtMs &&
               safety++ < 16) {
            timerFiredCalls++;
            if (link)
                link->onTimer();
        }
    }

    void runFor(uint32_t targetMs)
    {
        uint32_t end = now + targetMs;
        while (now < end) {
            uint32_t chunk = end - now;
            if (chunk > 100)
                chunk = 100;
            pumpClock(chunk);
        }
    }
    void delayMs(int) override {}
    void clearTx()
    {
        txBuf.clear();
        txBaudPerByte.clear();
    }

    uint32_t now = 0;
    int lastTimerMs = 0;
    uint32_t nowMs() override { return now; }

    size_t appBufCap = (size_t)-1;
    int txFailN = 0;

    void lock() const override { mtx.lock(); }
    void unlock() const override { mtx.unlock(); }

    int pushAppBuf(const uint8_t *b, int n) override
    {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            if (appBuf.size() >= appBufCap)
                break;
            appBuf.push(b[i]);
            acc++;
        }
        return acc;
    }
    int popAppBuf(uint8_t *b, int max_len) override
    {
        int n = 0;
        while (n < max_len && !appBuf.empty()) {
            b[n++] = appBuf.front();
            appBuf.pop();
        }
        return n;
    }
    int peekAppBuf() const override
    {
        if (appBuf.empty())
            return -1;
        return appBuf.front();
    }
    int peekAt(uint8_t *out, int n, int offset) const override
    {
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
    int appBufAvailable() const override { return appBuf.size(); }
    void clearAppBuf() override
    {
        while (!appBuf.empty())
            appBuf.pop();
    }
};

inline void pipe_data(MockHal &src, MockHal &dest)
{
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
    if (src.frameDropPct <= 0) {
        dest.link->onRx(kept.data(), kept.size());
        return;
    }

    uint32_t s = src.dropRngSeed ? src.dropRngSeed : 1;
    std::vector<uint8_t> delivered;
    size_t i = 0;
    while (i + 5 <= kept.size()) {
        bool drop = ((s = s * 1664525u + 1013904223u) % 100u) <
            (uint32_t)src.frameDropPct;
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
    if (!delivered.empty())
        dest.link->onRx(delivered.data(), delivered.size());
}

inline void negotiate_to_ok(Link &ping, Link &pong, MockHal &mHal,
                            MockHal &sHal)
{
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
        if (ping.getState() == State::OK && pong.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
}

} // namespace autolink

#endif