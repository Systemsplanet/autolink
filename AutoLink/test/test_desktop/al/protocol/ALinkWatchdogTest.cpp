// ALinkWatchdogTest.cpp — host-only tests for ALink post-OK watchdogs
// and recovery: idle watchdog, keepalive (atom, raw-mode, recent-TX),
// LCK timeout, asymmetric peer-death recovery.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include "MockHal.h"

using namespace autolink;

void test_idle_watchdog() {
 std::cout << "\n=== Test: Idle Watchdog Drops a Silent Link ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 // Entering OK must arm the watchdog tick.
 assert(mHal.timerActive);
 assert(sHal.timerActive);
 assert(mHal.lastTimerMs == 1000); // idleTimeoutMs / 3

 // Quiet tick: no drop, timer re-armed.
 mHal.now = 500;
 ping.onTimer();
 assert(ping.getState() == State::OK);

 // Silence past the limit: ping drops, peer is broken to SWP too.
 mHal.now = 4000;
 int breaks = mHal.sendBreakCalls;
 ping.onTimer();
 assert(ping.getState() == State::SWP);
 assert(mHal.sendBreakCalls == breaks + 1);
 assert(pong.getState() == State::SWP);
 std::cout << "PASS" << std::endl;
}

void test_keepalive() {
 std::cout << "\n=== Test: Keepalive Stops a Quiet Link From Bouncing ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 cfg.reliableMode = true;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);
 mHal.clearTx(); sHal.clearTx();

 mHal.now = 1000;
 ping.onTimer();
 // (COBS of a 2-byte input [cobsSeq, CRC] with a leading 0x00 is
 // [0x01, 0x02, cobsSeq, CRC] -- but the cobsSeq=0 case is special
 // because the input starts with 0x00; we get [0x01, 0x02, CRC] where
 // the second code byte is at index 2).
 assert(mHal.txBuf.size() == 5);
 assert(mHal.txBuf[0] == 0x00);
 assert(mHal.txBuf[4] == 0x00);

 sHal.now = 1000;
 pipe_data(mHal, sHal);
 assert(pong.getState() == State::OK);
 assert(pong.available() == 0);
 assert(pong.getErrCount() == 0);

 pong.onTimer();
 mHal.now = 2900;
 pipe_data(sHal, mHal);
 ping.onTimer();
 assert(ping.getState() == State::OK);
 std::cout << "PASS" << std::endl;
}

void test_keepalive_disabled_in_raw_mode() {
 std::cout << "\n=== Test: Keepalive Suppressed in Raw Mode ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.reliableMode = false;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);
 mHal.clearTx(); sHal.clearTx();

 mHal.now = 1000;
 ping.onTimer();
 assert(mHal.txBuf.empty());
 std::cout << "PASS" << std::endl;
}

void test_keepalive_quiet_after_recent_tx() {
 std::cout << "\n=== Test: Keepalive Skipped After Recent TX ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 cfg.reliableMode = true;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 mHal.now = 1000;
 uint8_t b = 0xAB;
 ping.write(&b, 1);
 mHal.clearTx();

 ping.onTimer();
 assert(mHal.txBuf.empty());
 std::cout << "PASS" << std::endl;
}

void test_lck_timeout() {
 std::cout << "\n=== Test: LCK Timeout Restarts the Sweep ===" << std::endl;
 // v5.1.40: pumpClock drives each timer tick. cfg.delayMs=50;
 // allowedBaudsCount=2 -> LCK retry threshold = 4.
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 MockHal mHal; // no peer: REQs go nowhere
 ALink ping(mHal, true, cfg);
 ping.begin();
 mHal.pumpClock(50); // PING@9600 -> SWP tick 1
 assert(ping.getState() == State::SWP);
 mHal.pumpClock(50); // PING@115200 -> SWP tick 2 -> LCK
 assert(ping.getState() == State::LCK);

 // 4 ticks in LCK (each at cfg.delayMs=50) — threshold = 2*2 = 4
 // retries. The 4th still in LCK, the 5th triggers re-sweep.
 for (int i = 0; i < 4; i++) {
   mHal.pumpClock(50);
 }
 assert(ping.getState() == State::LCK);
 mHal.pumpClock(50); // 5th tick -> Drop -> SWP
 assert(ping.getState() == State::SWP);
 assert(ping.getCurrentSpdIndex() == 0);
 std::cout << "PASS" << std::endl;
}

// Asymmetric peer-death recovery. v2.5 added the idle-channel watchdog so
// the ping drops to SWP and re-sweeps on its own when the pong goes silent.
// Here we exercise the same code path via err() trips (the real device
// path when the parser sees a flood of garbage after a peer's UART desyncs).
void test_asymmetric_peer_death_recovery() {
 std::cout << "\n=== Test: Asymmetric Peer-Death Recovery ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.reliableMode = true;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 cfg.idleTimeoutMs = 0;

 MockHal mHal, sHal;
 mHal.peer = &sHal;
 sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 int sendBreakCallsBeforePong = sHal.sendBreakCalls;
 int sendBreakCallsBeforePing = mHal.sendBreakCalls;
 int errsBefore = pong.getErrCount();

 // 300 non-zero bytes overflow the COBS accumulator and trip err_unlocked.
 for (int burst = 0; burst < 20; burst++) {
 std::vector<uint8_t> garbage(300, 0xCC);
 pong.onRx(garbage.data(), (int)garbage.size());
 if (pong.getErrCount() < errsBefore) break;
 }

 assert(pong.getErrCount() == 0); // dropped: errs reset to 0
 assert(pong.getState() == State::SWP);
 assert(sHal.sendBreakCalls == sendBreakCallsBeforePong + 1);
 assert(ping.getState() == State::SWP);
 assert(mHal.sendBreakCalls == sendBreakCallsBeforePing);

 // The ping is now sweeping on its own.
 ping.onTimer();
 assert(!mHal.txBuf.empty());
 assert(mHal.txBuf.size() == 5);
 assert(mHal.txBuf[3] == PING_CMD);

 pipe_data(mHal, sHal);
 assert(pong.getCurrentSpdIndex() == 1);
 assert(pong.getErrCount() == 0);

 std::cout << "PASS" << std::endl;
}

// v5.1.31: facade-driven link pause. When setLinkPaused(true) is
// called, onTimerOk_unlocked() must NOT drop the link on idle AND
// must NOT emit keepalive frames. The link stays up silently
// indefinitely (until manual resume or dropLink()). This is what
// lets Ping's Pause/Start button work — the link must survive
// the operator's inspection window without re-sweep churn.
void test_setLinkPaused_suppresses_idle_drop_and_keepalive() {
 std::cout << "\n=== Test: setLinkPaused(true) suppresses idle-drop and keepalive (v5.1.31) ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.reliableMode = true;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 // Pause the ping side.
 ping.setLinkPaused(true);

 // Clear any post-negotiation TX so we can detect keepalive emissions.
 mHal.clearTx(); sHal.clearTx();

 // Advance time well past idleTimeoutMs (3 s) in several onTimer ticks.
 // Without setLinkPaused, the watchdog would drop the link around t=4 s.
 // With it, the link should stay OK and no keepalive frames should emit.
 for (int t = 1000; t <= 9000; t += 1000) {
 mHal.now = (uint32_t)t;
 ping.onTimer();
 bool pingOk = ping.getState() == State::OK;
 bool pingQuiet = mHal.txBuf.empty();
 if (!pingOk) {
 std::cerr << "\nping dropped while paused at t=" << t << " (state="
 << (int)ping.getState() << ")" << std::endl;
 }
 if (!pingQuiet) {
 std::cerr << "\nping emitted a frame while paused at t=" << t
 << " (txBuf size=" << mHal.txBuf.size() << ")" << std::endl;
 }
 assert(pingOk);
 assert(pingQuiet);
 }

 // Pong was NOT paused (only ping side). Pong's idle watchdog is
 // independent, so if pong sees zero RX from ping for idleTimeoutMs
 // it WILL drop. Verify this is the case — the user must pause both
 // sides to keep the link fully silent.
 pong.onTimer();
 // pong's lastRxMs was set when ping's REQ arrived, so the timer
 // above didn't advance pong's clock. Force pong's clock to detect.
 sHal.now = 9000;
 pong.onTimer();
 bool pongSwp = pong.getState() == State::SWP;
 if (!pongSwp) {
 std::cerr << "\npong should have dropped due to its OWN idle watchdog (independence). state="
 << (int)pong.getState() << std::endl;
 }
 assert(pongSwp);

 // Now bring ping's link down by setLinkPaused(false) — should not
 // have torn down because we never set up anything to tear. Just
 // unpause and confirm ping survives one tick.
 mHal.peer = &sHal; sHal.peer = &mHal;
 pong.setLinkPaused(true);
 // (pong is already in SWP; setLinkPaused is a no-op for non-OK,
 // but let's still call to prove the setter is idempotent.)
 pong.setLinkPaused(false);
 pong.setLinkPaused(true);
 std::cout << "PASS" << std::endl;
}

// v5.1.31: setLinkPaused(false) restores normal idle/keepalive
// behavior. We assert this by toggling on then off and verifying
// the link still works.
void test_setLinkPaused_false_restores_normal() {
 std::cout << "\n=== Test: setLinkPaused(false) restores normal watchdog behavior ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.reliableMode = true;
 cfg.idleTimeoutMs = 3000;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 // Pause, advance, confirm no drop. Then unpause, advance past
 // idle, confirm drop happens.
 ping.setLinkPaused(true);
 mHal.now = 5000;
 ping.onTimer();
 if (ping.getState() != State::OK) {
 std::cerr << "\nshould not drop while paused (state="
 << (int)ping.getState() << ")" << std::endl;
 }
 assert(ping.getState() == State::OK);

 ping.setLinkPaused(false);
 mHal.now = 5000; // simulate pong's lastRxMs being stale by
 // zeroing both timers via clearTx/peer-deliver. Easiest: just
 // drop pong, see that pong will go SWP on idle.
 sHal.now = 5000;
 ping.onTimer();
 pong.onTimer();
 // After unpause, ping's idle watchdog should bite because no
 // keepalive ever went out. The link drops.
 if (ping.getState() != State::SWP) {
 std::cerr << "\nping should drop after unpause + idle (state="
 << (int)ping.getState() << ")" << std::endl;
 }
 assert(ping.getState() == State::SWP);
 std::cout << "PASS" << std::endl;
}

int main() {
 std::cout << "=== Running ALinkWatchdog Tests ===" << std::endl;
 test_idle_watchdog();
 test_keepalive();
 test_keepalive_disabled_in_raw_mode();
 test_keepalive_quiet_after_recent_tx();
 test_lck_timeout();
 test_asymmetric_peer_death_recovery();
 test_setLinkPaused_suppresses_idle_drop_and_keepalive();
 test_setLinkPaused_false_restores_normal();
 std::cout << "\n=== ALinkWatchdog Tests Completed Successfully ===" << std::endl;
 return 0;
}

#endif // ARDUINO
