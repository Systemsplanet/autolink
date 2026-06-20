// MockHal.h — host-side ILink mock + pipe_data helper, shared by every
// ALink / AutoLink host test.
//
// MockHal records TX bytes / breaks / baud changes, exposes an injectable
// clock and an optional app-buffer capacity, and can deliver BREAKs to a
// peer MockHal to mirror real wire semantics. Tests #include this header
// after ALink.h so the ILink interface resolves.
#pragma once
#ifndef ARDUINO

#include "al/protocol/ALink.h"
#include <queue>
#include <mutex>
#include <vector>
#include <cstdint>
#include <cassert>

namespace autolink {

class MockHal : public ILink {
public:
 uint32_t spd = 9600;
 bool timerActive = false;
 std::vector<uint8_t> txBuf;

 int sendBreakCalls = 0;
 int timerStartCalls = 0;
 std::vector<uint32_t> spdHistory;

 // Optional peer pointer used by the asymmetric-recovery tests. On real
 // hardware, sendBreak() puts a break on the TX wire and the *other* ESP32
 // receives it on its RX pin asynchronously. When peer is set, sendBreak
 // delivers onBreak to the peer's link instead of self, mirroring the
 // wire-level semantics.
 MockHal* peer = nullptr;

 // Wire-noise injection knob for protocol-level loopback tests. When set
 // to a value in [0,100], pipe_data() will drop that percentage of frames
 // before delivering them to the peer's link. Drop a *frame* (whole
 // COBS-encoded packet) rather than random bytes because that's what real
 // wire glitches look like — a burst of noise eats a packet, not part of
 // one. Defaults to 0 (clean wire, every prior test's behavior).
 int frameDropPct = 0;
 int bytesDropped = 0; // running counter of bytes lost to noise
 uint32_t dropRngSeed = 1; // deterministic — same seed = same drops every run

 std::queue<uint8_t> appBuf;
 mutable std::mutex mtx;

 void begin() override {}
 void setSpd(uint32_t s) override { spd = s; spdHistory.push_back(s); }
 void sendBreak() override {
 sendBreakCalls++;
 if (peer && peer->link) peer->link->onBreak();
 }
 void deliver_break_to_self() { if (link) link->onBreak(); }
 int tx(const uint8_t* b, int n) override {
 // tx() returns short by `txFailN` bytes. Used by the coverage
 // test to exercise the TX-truncated error paths in
 // sendCobsFrame_unlocked and write().
 if (txFailN > 0) {
 int take = n > txFailN ? n - txFailN : 0;
 if (take > 0) txBuf.insert(txBuf.end(), b, b+take);
 txFailN = 0;
 return take;
 }
 txBuf.insert(txBuf.end(), b, b+n);
 return n; // mock always accepts all bytes
 }
 void flushTx() override {}
 void startTimer(int ms) override { timerStartCalls++; timerActive = true; lastTimerMs = ms; }
 void stopTimer() override { timerActive = false; }
 void delayMs(int) override {}
 void clearTx() { txBuf.clear(); }

 // Injectable clock so host tests can drive the idle watchdog/keepalive.
 uint32_t now = 0;
 int lastTimerMs = 0;
 uint32_t nowMs() override { return now; }

 // Optional app-buffer capacity to simulate stream-buffer overflow.
 size_t appBufCap = (size_t)-1;
 int txFailN = 0; //: 0=ok; >0 = next tx() returns short by N

 void lock() const override { mtx.lock(); }
 void unlock() const override { mtx.unlock(); }

 int pushAppBuf(const uint8_t* b, int n) override {
 int acc = 0;
 for (int i = 0; i < n; i++) {
 if (appBuf.size() >= appBufCap) break;
 appBuf.push(b[i]);
 acc++;
 }
 return acc;
 }
 int popAppBuf(uint8_t* b, int max_len) override {
 int n = 0;
 while(n < max_len && !appBuf.empty()) {
 b[n++] = appBuf.front();
 appBuf.pop();
 }
 return n;
 }
 int peekAppBuf() const override {
 if (appBuf.empty()) return -1;
 return appBuf.front();
 }
 // the front-of-queue bytes without consuming them.
 int peekAt(uint8_t* out, int n, int offset) const override {
 if (n <= 0 || offset < 0 || offset >= (int)appBuf.size()) return 0;
 int copied = 0;
 int idx = 0;
 // std::queue doesn't expose indexed access; copy to a
 // scratch vector, then index.
 std::vector<uint8_t> tmp(appBuf.size());
 int i = 0;
 std::queue<uint8_t> copy = appBuf;
 while (!copy.empty()) { tmp[i++] = copy.front(); copy.pop(); }
 for (int k = offset; k < (int)tmp.size() && copied < n; k++) {
 out[copied++] = tmp[k];
 }
 (void)idx;
 return copied;
 }
 int appBufAvailable() const override { return appBuf.size(); }
 void clearAppBuf() override {
 while(!appBuf.empty()) appBuf.pop();
 }
};

// Hand a src MockHal's pending TX bytes to dest's link, as if they
// arrived on the wire. Used by every test that exercises the protocol
// across a paired ping/pong.
//
// Frame-level drop: if src.frameDropPct is set, each frame in the buffer
// (delimited by the 0x00 COBS sentinel) has `frameDropPct`% chance of
// being dropped entirely before being delivered to dest's onRx. This
// simulates wire noise that loses whole packets (a burst of EMI) without
// corrupting individual bytes mid-packet. Used by
// test_loopback_noise_triggers_baud_fallback to verify the protocol's
// error-threshold / re-sweep path actually fires under noise.
inline void pipe_data(MockHal& src, MockHal& dest) {
 if (src.txBuf.empty()) return;
 if (src.frameDropPct <= 0) {
 dest.link->onRx(src.txBuf.data(), src.txBuf.size());
 src.clearTx();
 return;
 }
 // Deterministic LCG so test failures are reproducible. We advance the
 // seed for every frame we *consider* (drop or keep) and drop when
 // (seed % 100) < frameDropPct.
 uint32_t s = src.dropRngSeed ? src.dropRngSeed : 1;
 std::vector<uint8_t> kept;
 kept.reserve(src.txBuf.size());
 // Walk frames delimited by 0x00; deliver only the kept ones.
 size_t i = 0;
 while (i < src.txBuf.size()) {
 size_t end = i;
 while (end < src.txBuf.size() && src.txBuf[end] != 0x00) end++;
 // [i, end) is one frame's payload. The trailing 0x00 (if any) is the
 // COBS end-marker.
 size_t frameEnd = end;
 if (frameEnd < src.txBuf.size()) frameEnd++; // include the sentinel
 bool drop = ((s = s * 1664525u + 1013904223u) % 100u) < (uint32_t)src.frameDropPct;
 if (drop) {
 src.bytesDropped += (int)(frameEnd - i);
 } else {
 kept.insert(kept.end(), src.txBuf.begin() + i, src.txBuf.begin() + frameEnd);
 }
 i = frameEnd;
 }
 src.dropRngSeed = s;
 src.clearTx();
 if (!kept.empty()) dest.link->onRx(kept.data(), kept.size());
}

// Negotiate a ping/pong MockHal pair into State::OK via the legacy
// "one PING per baud, then REQ_CMD" path. Used by every test that
// needs to start in OK before exercising the post-negotiation API.
inline void negotiate_to_ok(ALink& ping, ALink& pong, MockHal& mHal, MockHal& sHal) {
 ping.begin();
 pong.begin();
 ping.onTimer(); pipe_data(mHal, sHal); // PING@baud[0]
 ping.onTimer(); pipe_data(mHal, sHal); // PING@baud[1] -> LCK
 ping.onTimer(); pipe_data(mHal, sHal); // REQ -> pong OK, replies index
 pipe_data(sHal, mHal); // ping receives index -> OK
 assert(ping.getState() == State::OK);
 assert(pong.getState() == State::OK);
}

} // namespace autolink

#endif // !ARDUINO
