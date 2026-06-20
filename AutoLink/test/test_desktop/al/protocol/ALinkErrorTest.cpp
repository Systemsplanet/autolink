// ALinkErrorTest.cpp — host-only tests for the ALink error / threshold /
// counter paths: custom thresholds, lifetime disconnect count, app-buffer
// overflow, scattered-errors tolerance, parser yields after drop, and the
// link-failure regression suite.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

void test_error_threshold() {
 std::cout << "\n=== Test: Custom Error Thresholding ===" << std::endl;
 MockHal mHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.errThreshold = 2; cfg.pingSamplesPerBaud = 1;
 ALink ping(mHal, true, cfg);

 assert(ping.getState() == State::OK);
 ping.err();
 assert(ping.getState() == State::OK);
 assert(ping.getErrCount() == 1);

 ping.clearErr();
 assert(ping.getErrCount() == 0);

 ping.err();
 ping.err();
 assert(ping.getState() == State::OK);
 assert(ping.getErrCount() == 2);

 ping.err();
 assert(ping.getState() == State::SWP);
 std::cout << "PASS" << std::endl;
}

void test_error_counter() {
 // The disconnect counter is exactly: 1 per OK->SWP transition.
 // Per-byte error noise does NOT count (that's the parser's job and
 // would make the counter useless for longevity testing).
 std::cout << "\n=== Test: Disconnect Counter = One Per Link Drop ===" << std::endl;

 // Part 1: no drops = no counts, even with corrupt bytes flying around.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 cfg.errThreshold = 1000; // keep the link up
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);
 uint8_t rx[32];
 Stats bs;

 b.getStats(bs);
 assert(bs.discCount == 0);

 for (int k = 0; k < 10; k++) {
 mHal.txBuf.clear();
 uint8_t m[] = {(uint8_t)k, 0xAA, 0xBB};
 assert(a.sendMsg(m, 3));
 mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x80;
 pipe_data(mHal, sHal);
 b.recvMsg(rx, sizeof(rx));
 }
 b.getStats(bs);
 assert(bs.discCount == 0);
 }
 (void)0;

 // Part 2: each forced drop = exactly one count.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 cfg.errThreshold = 2; // 3 errs trips the threshold
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);
 Stats bs;

 b.getStats(bs);
 assert(bs.discCount == 0);

 for (int i = 0; i < 6; i++) b.err();
 assert(b.getState() == State::SWP);
 b.getStats(bs);
 assert(bs.discCount == 1);

 // Post-drop noise: err() while in SWP is a no-op.
 for (int i = 0; i < 100; i++) b.err();
 b.getStats(bs);
 assert(bs.discCount == 1);

 // resetStats() leaves the disconnect counter alone.
 b.resetStats();
 b.getStats(bs);
 assert(bs.discCount == 1);

 // resetErrors() zeros it.
 b.resetErrors();
 b.getStats(bs);
 assert(bs.discCount == 0);
 }

 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 Stats as;
 a.getStats(as);
 (void)as.tx; (void)as.rx;
 }

 std::cout << "PASS" << std::endl;
}

void test_error_counter_during_swp() {
 // Regression: a cable bounce drops the link, the ping spends a few
 // seconds in SWP/LCK re-locking, and recovers. With the per-drop
 // semantic, this is ONE disconnect event -- not N+1 from the noise
 // bytes that arrive during the sweep. The threshold window (`errs`)
 // resets after a drop, so post-drop noise during SWP cannot itself
 // trip a second drop until errs reaches threshold again.
 std::cout << "\n=== Test: One Count Per Cable Bounce ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.errThreshold = 2; // 3 errs trips the threshold
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 Stats s0;
 ping.getStats(s0);
 assert(s0.discCount == 0);

 for (int i = 0; i < 6; i++) ping.err();
 assert(ping.getState() == State::SWP);
 ping.getStats(s0);
 assert(s0.discCount == 1);

 for (int i = 0; i < 100; i++) ping.err();
 ping.getStats(s0);
 assert(s0.discCount == 1);

 for (int i = 0; i < 3; i++) {
 mHal.pumpClock(50); ping.onTimer();
 if (!mHal.txBuf.empty()) pipe_data(mHal, sHal);
 }
 ping.getStats(s0);
 assert(s0.discCount == 1);

 std::cout << "PASS" << std::endl;
}

void test_error_counter_link_failures() {
 // Regression: a cable bounce / silent peer death / no-reply LCK are all
 // real link failures and must bump the lifetime error counter, even
 // though recovery is clean. Before this fix, only the parser's per-byte
 // err_unlocked() path counted, which meant a perfect re-sweep after a
 // watchdog trip showed err=0 -- exactly what the user reported.
 std::cout << "\n=== Test: Error Counter Ticks on Link Failures ===" << std::endl;

 // Case 1: begin() must NOT count as an error.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 ping.begin(); pong.begin();
 Stats s0;
 ping.getStats(s0);
 assert(s0.discCount == 0);
 }

 // Case 2: idle watchdog trip counts as exactly one.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.idleTimeoutMs = 100;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 mHal.now = cfg.idleTimeoutMs + 50;
 mHal.pumpClock(50); ping.onTimer();
 assert(ping.getState() == State::SWP);

 Stats s1;
 ping.getStats(s1);
 assert(s1.discCount == 1);
 }

 // Case 3: peer's BREAK arriving on us counts as exactly one.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 ping.onBreak();
 assert(ping.getState() == State::SWP);

 Stats s;
 ping.getStats(s);
 assert(s.discCount == 1);
 }

 // Case 4: an OK -> SWP transition counts; SWP/LCK recovery noise does
 // not inflate the count.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.errThreshold = 2; // 3 errs trips the threshold
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 for (int i = 0; i < 6; i++) ping.err();
 assert(ping.getState() == State::SWP);
 Stats s;
 ping.getStats(s);
 assert(s.discCount == 1);

 // v5.1.40: pumpClock drives each tick at cfg.delayMs=50.
 for (int i = 0; i < 100; i++) {
   mHal.pumpClock(50);
   mHal.pumpClock(50); ping.onTimer();
 }
 Stats s2;
 ping.getStats(s2);
 assert(s2.discCount == 1);
 }

 // Case 5: cable-bounce simulation. begin, negotiate, bounce the pong
 // (silent past idleTimeout), let ping recover. Expect exactly one count.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.idleTimeoutMs = 100;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 mHal.now = cfg.idleTimeoutMs + 50;
 mHal.pumpClock(50); ping.onTimer();
 assert(ping.getState() == State::SWP);

 for (int i = 0; i < 20; i++) ping.err();
 for (int i = 0; i < 5; i++) ping.err();

 // Pong "comes back": finish the re-sweep and re-lock.
 mHal.pumpClock(50); ping.onTimer(); pipe_data(mHal, sHal);
 mHal.pumpClock(50); ping.onTimer(); pipe_data(mHal, sHal);
 mHal.pumpClock(50); ping.onTimer(); pipe_data(mHal, sHal);
 pipe_data(sHal, mHal);

 Stats s;
 ping.getStats(s);
 assert(s.discCount == 1);
 }

 // Case 6: a pong reset that emits many BREAKs in a row while the ping
 // is in SWP should still count as ONE event. The user's log showed
 // err=9 for one reset; the new rule brings this to 1.
 {
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 ping.onBreak();
 assert(ping.getState() == State::SWP);
 for (int i = 0; i < 5; i++) ping.onBreak(); // spurious, in SWP
 Stats s;
 ping.getStats(s);
 assert(s.discCount == 1);

 // v5.1.40: pumpClock drives each tick at cfg.delayMs=50.
 for (int i = 0; i < 200; i++) {
   mHal.pumpClock(50);
   mHal.pumpClock(50); ping.onTimer();
 }
 ping.getStats(s);
 assert(s.discCount == 1);
 }

 std::cout << "PASS" << std::endl;
}

void test_app_buffer_overflow_does_not_drop_link() {
 // a wire error. Previously this test verified that app-buffer-full
 // does NOT count -- the link stays in OK. The dropped frame shows up
 std::cout << "\n=== Test: App Buffer Overflow Does Not Drop Link ===" << std::endl;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.errThreshold = 2;
 cfg.streamBufferSize = 256;
 MockHal mHal, sHal;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);
 sHal.appBufCap = 4; // very small app buffer -- overflows on every frame

 uint8_t msg[64];
 for (int i = 0; i < 64; i++) msg[i] = (uint8_t)i;

 // Send several messages. The first 1-2 will partially fit in the
 // this would count toward errThreshold and drop the link after a
 // stays in OK.
 for (int k = 0; k < 5; k++) {
 if (!a.sendMsg(msg, 64)) break;
 pipe_data(mHal, sHal);
 if (b.getState() != State::OK) break;
 }
 assert(b.getState() == State::OK);
 assert(b.getErrCount() == 0);
 { Diag d; b.getDiag(d); assert(d.gaps > 0); }
 std::cout << "PASS" << std::endl;
}

// Regression: a good frame must reset the consecutive-error counter, so
// occasional CRC rejects scattered between healthy traffic never drop a
// working link. Only a genuine *run* of back-to-back errors should trip the
// threshold.
void test_scattered_errors_dont_drop() {
 std::cout << "\n=== Test: Scattered Errors Don't Drop a Working Link ===" << std::endl;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.errThreshold = 5;
 cfg.streamBufferSize = 8192;
 MockHal mHal, sHal;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // CRC-only frame: a single 0x00 delimiter pair around one byte decodes
 // to a payload whose CRC can't match -> exactly one onFrameError.
 uint8_t badFrame[] = {0x00, 0x02, 0xFF, 0x00};
 uint8_t msg[] = {0x11, 0x22, 0x33, 0x44};

 for (int k = 0; k < 20; k++) {
 b.onRx(badFrame, sizeof(badFrame));
 assert(b.getState() == State::OK);
 assert(a.sendMsg(msg, sizeof(msg)));
 pipe_data(mHal, sHal);
 assert(b.getState() == State::OK);
 uint8_t rx[16];
 assert(b.recvMsg(rx, sizeof(rx)) == (int)sizeof(msg));
 assert(b.getErrCount() == 0);
 }

 for (int k = 0; k <= (int)cfg.errThreshold; k++) b.onRx(badFrame, sizeof(badFrame));
 assert(b.getState() == State::SWP);
 std::cout << "PASS" << std::endl;
}

// After the err threshold trips mid-event, the rest of the same UART event
// must be handed to the command parser, not consumed as OK-mode frame bytes.
void test_parser_yields_after_drop() {
 std::cout << "\n=== Test: Parser Yields to Command Parser After Drop ===" << std::endl;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.errThreshold = 1;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 MockHal mHal, sHal;
 sHal.peer = &mHal;
 ALink pingNode(mHal, true, cfg);
 ALink pong(sHal, false, cfg);

 // Two bad frames trip threshold 1 (errs > 1), then a valid PING follows.
 uint8_t bad[] = {0x00, 0x02, 0xFF, 0x00, 0x02, 0xFF, 0x00};
 uint8_t ping[5] = {0xAA, 0x55, /*cobsSeq=*/0, PING_CMD, 0};
 // CRC8 of the first 4 bytes (poly 0x07).
 uint8_t crc = 0;
 for (int i = 0; i < 4; i++) {
 crc ^= ping[i];
 for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
 }
 ping[4] = crc;

 std::vector<uint8_t> event(bad, bad + sizeof(bad));
 event.insert(event.end(), ping, ping + 5);
 pong.onRx(event.data(), (int)event.size());

 assert(pong.getState() == State::SWP);
 // PING at the tail must have been scored by the command parser.
 assert(pong.getCurrentSpdIndex() == 1);
 std::cout << "PASS" << std::endl;
}

int main() {
 std::cout << "=== Running ALinkError Tests ===" << std::endl;
 test_error_threshold();
 test_error_counter();
 test_error_counter_during_swp();
 test_error_counter_link_failures();
 test_app_buffer_overflow_does_not_drop_link();
 test_scattered_errors_dont_drop();
 test_parser_yields_after_drop();
 std::cout << "\n=== ALinkError Tests Completed Successfully ===" << std::endl;
 return 0;
}

#endif // ARDUINO
