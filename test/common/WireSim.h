// Closed-loop wire simulator: two IHal adapters
// talking to each other via two memory pipes; MockHal
// drives the clock. Lets the host test suite run real
// Link ↔ Link traffic with no hardware.
#pragma once
#ifndef AUTOLINK_HOST_TEST
#    error \
        "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "MockHal.h"
#include "AutoLink.h"

#include <cstdint>
#include <vector>
#include <cassert>
#include <iostream>

namespace autolink
{
class WireSim
{
public:
    WireSim(AutoLinkConfig cfg = AutoLinkConfig())
        : mA_(new MockHal()), mB_(new MockHal()),
          a_(mA_.get(), true, cfg),
          b_(mB_.get(), false, cfg)
    {
        mA_->peer = mB_.get();
        mB_->peer = mA_.get();

        mA_->now = 0;
        mB_->now = 0;
    }


    void setFrameDropPct(int pct)
    {
        mA_->frameDropPct = pct;
        mB_->frameDropPct = pct;
    }


    void setForcedDropEvery(int n)
    {
        forcedDropEvery_ = n;
    }


    void requestDrop() { dropPending_ = true; }


    MockHal &halA() { return *mA_; }
    MockHal &halB() { return *mB_; }
    AutoLink &linkA() { return a_; }
    AutoLink &linkB() { return b_; }


    void step(uint32_t deltaMs)
    {
        Stats preA, preB;
        a_.linkForTest()->getStats(preA);
        b_.linkForTest()->getStats(preB);


        if (dropPending_ ||
            (forcedDropEvery_ > 0 &&
             (++stepCount_) % forcedDropEvery_ == 0)) {
            if (dropPending_ ||
                (forcedDropEvery_ > 0)) {
                a_.dropLink();
                dropsInjected_++;
                dropPending_ = false;
            }
        }


        int fireCountA = 0, fireCountB = 0;
        mA_->now += deltaMs;
        mB_->now += deltaMs;
        while ((mA_->nextTimerAtMs != UINT32_MAX &&
                mA_->now >= mA_->nextTimerAtMs) &&
               fireCountA++ < 32) {
            mA_->timerFiredCalls++;
            a_.linkForTest()->onTimer();
        }
        while ((mB_->nextTimerAtMs != UINT32_MAX &&
                mB_->now >= mB_->nextTimerAtMs) &&
               fireCountB++ < 32) {
            mB_->timerFiredCalls++;
            b_.linkForTest()->onTimer();
        }


        pipe_data(*mA_, *mB_);
        pipe_data(*mB_, *mA_);

        Stats postA, postB;
        a_.linkForTest()->getStats(postA);
        b_.linkForTest()->getStats(postB);
        int protoDropsThisStep =
            (int)((postA.discCount - preA.discCount) +
                  (postB.discCount - preB.discCount));
        if (protoDropsThisStep > 0)
            protoDropsSeen_ += protoDropsThisStep;
    }


    uint64_t bytesTransferredAtoB() const
    {
        Stats sa, sb;
        a_.linkForTest()->getStats(sa);
        b_.linkForTest()->getStats(sb);


        return sb.rx;
    }
    uint64_t bytesTransferredBtoA() const
    {
        Stats sa, sb;
        a_.linkForTest()->getStats(sa);
        b_.linkForTest()->getStats(sb);
        return sa.rx;
    }

    int dropsInjected() const
    {
        return dropsInjected_;
    }
    int protoDropsSeen() const
    {
        return protoDropsSeen_;
    }
    int framesDropped() const
    {
        return mA_->bytesDropped + mB_->bytesDropped;
    }
    int pendingCountA() const
    {
        return a_.arqCacheSizeForTest();
    }
    int pendingCountB() const
    {
        return b_.arqCacheSizeForTest();
    }
    State getStateA() const { return a_.getState(); }
    State getStateB() const { return b_.getState(); }


    AutoLink &nodeAForTest() { return a_; }
    AutoLink &nodeBForTest() { return b_; }


    const MockHal &rawA() const { return *mA_; }
    const MockHal &rawB() const { return *mB_; }

private:
    std::unique_ptr<MockHal> mA_;
    std::unique_ptr<MockHal> mB_;
    AutoLink a_;
    AutoLink b_;

    int forcedDropEvery_ = 0;
    int stepCount_ = 0;
    bool dropPending_ = false;
    int dropsInjected_ = 0;


    uint64_t lastDiscA_ = 0;
    uint64_t lastDiscB_ = 0;
    int protoDropsSeen_ = 0;
};

class TwoNodeFixture
{
public:
    explicit TwoNodeFixture(WireSim &sim) : sim_(sim)
    {
        pendingA_.assign(32, Slot{ false, 0, 0, 0 });
        pendingB_.assign(32, Slot{ false, 0, 0, 0 });
    }


    AutoLink &nodeA() { return sim_.nodeAForTest(); }
    AutoLink &nodeB() { return sim_.nodeBForTest(); }


    void begin()
    {
        sim_.linkA().begin();
        sim_.linkB().begin();
    }


    uint64_t step(uint32_t deltaMs)
    {
        int dropsBefore = sim_.dropsInjected() +
            sim_.protoDropsSeen();
        sim_.step(deltaMs);
        int dropsAfter = sim_.dropsInjected() +
            sim_.protoDropsSeen();
        if (dropsAfter > dropsBefore) {
            for (auto &s : pendingA_)
                s.active = false;
            for (auto &s : pendingB_)
                s.active = false;
        }


        drivePing_();

        drivePong_();
        return sim_.bytesTransferredAtoB();
    }


    uint64_t totalBytesAtoB() const
    {
        return sim_.bytesTransferredAtoB();
    }


    WireSim &sim() { return sim_; }
    const MockHal &halA() { return sim_.rawA(); }
    const MockHal &halB() { return sim_.rawB(); }
    int pendingCountA()
    {
        sim_.halA().now;
        return sim_.pendingCountA();
    }
    int pendingCountB()
    {
        sim_.halB().now;
        return sim_.pendingCountB();
    }
    State getStateA() const
    {
        return sim_.getStateA();
    }
    State getStateB() const
    {
        return sim_.getStateB();
    }


    int maxBurstPerLoop = 4;


    int msgSize = 64;
    int windowSize = 32;
    bool debug_log_ = false;

private:
    struct Slot {
        bool active;
        int len;
        uint16_t crc;
        uint32_t sentMs;
    };
    std::vector<Slot> pendingA_;
    std::vector<Slot> pendingB_;

    WireSim &sim_;
    uint32_t seqSeed_ = 0x12345;

    uint8_t nextByte_()
    {
        seqSeed_ = seqSeed_ * 1103515245u + 12345u;
        return (uint8_t)(seqSeed_ >> 16);
    }

    void fillPayload_(uint8_t *buf, int n)
    {
        for (int i = 0; i < n; i++)
            buf[i] = nextByte_();
    }


    void drivePing_()
    {
        if (sim_.getStateA() != State::OK)
            return;

        uint8_t buf[1024];
        int n;
        while ((n = sim_.linkA().recvMsg(
                    buf, sizeof buf)) > 0) {
            for (auto &s : pendingA_) {
                if (s.active && s.len == n) {
                    s.active = false;
                    break;
                }
            }
        }


        int sent = 0;
        for (auto &s : pendingA_) {
            if (sent >= maxBurstPerLoop)
                break;
            if (s.active)
                continue;
            int len = msgSize;
            fillPayload_(buf, len);
            bool ok = sim_.linkA().sendMsg(buf, len);
            if (debug_log_) {
                std::cerr << "[drivePing_] sendMsg("
                          << len << ") -> "
                          << (ok ? "true" : "false")
                          << " stateA="
                          << (int)sim_.getStateA()
                          << " facadeCache="
                          << sim_.pendingCountA()
                          << std::endl;
            }
            if (ok) {
                s.active = true;
                s.len = len;
                s.crc = 0;
                s.sentMs = sim_.halA().now;
                sent++;
            } else {
                break;
            }
        }
        if (debug_log_) {
            std::cerr
                << "[drivePing_] sent=" << sent
                << " pendingA_=" << pendingA_.size()
                << std::endl;
        }
    }


    void drivePong_()
    {
        if (sim_.getStateB() != State::OK)
            return;
        uint8_t buf[1024];
        int n;
        int sent = 0;
        while ((n = sim_.linkB().recvMsg(
                    buf, sizeof buf)) > 0 &&
               sent < maxBurstPerLoop) {
            if (sim_.linkB().sendMsg(buf, n)) {
                sent++;
            }
        }
    }
};

} // namespace autolink
