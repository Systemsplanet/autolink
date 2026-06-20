// Host-only unit tests for the AutoLink facade.
//
// AutoLink's constructor hardcodes EspHal (a FreeRTOS-dependent class)
// and EspBlinkHal (uses esp_timer). To host-test the facade we provide
// minimal no-op stubs that satisfy the type signatures, then exercise
// the public API surface. The actual protocol / LED logic is covered
// separately by ALink tests (test.cpp) and UtilBlink tests
// (UtilBlinkTest.cpp); the embedded test (test_embedded.ino) covers
// the full hardware path.
//
// Build: -DAUTOLINK_HOST_TEST must be set. See Makefile.

// ---------------------------------------------------------------------------
// Host stubs -- defined BEFORE AutoLink.h is included.
// ---------------------------------------------------------------------------
#ifndef AUTOLINK_HOST_TEST
#error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/hal/ILink.h"
#include "al/protocol/ALink.h" // pulls in AutoLinkConfig used by the EspHal stub ctor
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"

// ---------------------------------------------------------------------------
// AutoLink facade tests.
// ---------------------------------------------------------------------------
#include "AutoLink.h"
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <utility>

using namespace autolink;

void test_default_construction() {
 std::cout << "\n=== Test: Default Construction ===" << std::endl;
 AutoLink link(0, 16, 17, /*isMaster=*/true);
 (void)link;
 std::cout << "PASS" << std::endl;
}

void test_custom_config_construction() {
 std::cout << "\n=== Test: Custom Config Construction ===" << std::endl;
 AutoLinkConfig cfg;
 cfg.ledPin = 4;
 cfg.maxMsg = 4096;
 cfg.idleTimeoutMs = 5000;
 AutoLink link(0, 16, 17, /*isMaster=*/false, cfg);
 (void)link;
 std::cout << "PASS" << std::endl;
}

// UtilPing WINDOW of messages plus Pong's MAX_TX_PER_LOOP echo headroom.
// Before this change, the default was 2 * (maxMsg + MSG_HDR) which was too
// small for the Ping/Pong example: Ping's 8-message pipeline (~8 KB) would
// overflow Pong's 2 KB app buffer at link-up, which the old code treated
// as a wire error and dropped the link. Verify the new auto-size.
void test_app_buffer_auto_sized_for_pingpong() {
 std::cout << "\n=== Test: App Buffer Auto-Sized for Ping/Pong (1,) ===" << std::endl;
 // xStreamBufferCreate to fail on fragmented heaps, leaving the
 // stream_buf NULL and producing the "app buffer full" symptom on
 // the first frame after link-up. The bound that actually matters
 // is the per-tick send/drain imbalance, which is at most
 // MAX_TX_PER_LOOP. So 2 * MAX_TX_PER_LOOP = 32 frames is the
 // right size. With default maxMsg=1024 and MSG_HDR=6, that's
 // holds (>= (8+2) * 1030 = 10300).
 AutoLinkConfig cfg;
 cfg.maxMsg = 1024; // default for UtilPing/UtilPong
 AutoLink link(0, 16, 17, /*isMaster=*/false, cfg);
 const size_t MAX_TX_PER_LOOP = 16; //: was 4
 const size_t MSG_HDR = 6;
 const size_t expected_min = 2 * MAX_TX_PER_LOOP * (cfg.maxMsg + MSG_HDR);
 assert(expected_min == 32960); // 32 * 1030
 assert(link.getStreamBufferSize() >= expected_min);
 assert(link.getStreamBufferSize() >= (8 + 2) * 1030);
 std::cout << "PASS (streamBufferSize=" << link.getStreamBufferSize() << " min=" << expected_min << ")" << std::endl;
}

void test_state_api() {
 std::cout << "\n=== Test: State API ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 (void)link.getState();
 (void)link.getCurrentBaud();
 (void)link.ready();
 (void)link.getErrCount();
 { Stats s; link.getStats(s); (void)s.frameErrs; }
 std::cout << "PASS" << std::endl;
}

void test_stats_api() {
 std::cout << "\n=== Test: Stats API ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 Stats s;
 link.getStats(s);
 link.resetStats();
 link.resetErrors();
 std::cout << "PASS" << std::endl;
}

void test_stream_api() {
 std::cout << "\n=== Test: Stream API ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 (void)link.available();
 (void)link.peek();
 link.flush();
 (void)link.write((uint8_t)0xAB);
 uint8_t wb[] = {0xCD, 0xEF};
 (void)link.write(wb, 2);
 uint8_t rb[8];
 (void)link.read(rb, 8);
 (void)link.read();
 std::cout << "PASS" << std::endl;
}


void test_message_api() {
 std::cout << "\n=== Test: Message API ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 uint8_t msg[] = {0x10, 0x20, 0x30};
 (void)link.send(msg, 3);
 uint8_t buf[16];
 (void)link.recv(buf, sizeof buf);
 (void)link.sendMsg(msg, 3);
 (void)link.recvMsg(buf, sizeof buf);
 std::cout << "PASS" << std::endl;
}

void test_err_clearing() {
 std::cout << "\n=== Test: Error Control API ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 link.err();
 link.err();
 (void)link.getErrCount();
 link.clearErr();
 std::cout << "PASS" << std::endl;
}

void test_droplink_safe_before_begin() {
 std::cout << "\n=== Test: dropLink Safe Before begin() ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 link.dropLink();
 std::cout << "PASS" << std::endl;
}

void test_blink_async_returns_immediately() {
 std::cout << "\n=== Test: blinkWait Async Returns Quickly ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 link.blinkWait(3, 100, 100, 0);
 std::cout << "PASS" << std::endl;
}

void test_blink_invalid_ignored() {
 std::cout << "\n=== Test: blinkWait Invalid n Is Ignored ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 link.blinkWait(0);
 link.blinkWait(-5);
 std::cout << "PASS" << std::endl;
}

void test_ishealthy_default() {
 std::cout << "\n=== Test: isHealthy Default ===" << std::endl;
 AutoLink link(0, 16, 17, true);
 (void)link.isHealthy();
 std::cout << "PASS" << std::endl;
}

void test_non_copyable() {
 // The class deletes copy ctor and copy assignment. The only way to
 // "move" an AutoLink is through the explicitly-defaulted move ops;
 // since the class holds unique_ptrs it is non-copyable. This is
 // enforced at compile time -- if a copy were attempted, the build
 // would fail. We construct two separate instances here to confirm
 // the API works for independent objects.
 std::cout << "\n=== Test: AutoLink Constructible Per-Instance ===" << std::endl;
 AutoLink a(0, 16, 17, true);
 AutoLink b(0, 16, 17, false);
 (void)a; (void)b;
 std::cout << "PASS" << std::endl;
}

int main() {
 std::cout << "=== Running AutoLink Facade Tests ===" << std::endl;
 test_default_construction();
 test_custom_config_construction();
 test_app_buffer_auto_sized_for_pingpong();
 test_state_api();
 test_stats_api();
 test_stream_api();
 test_message_api();
 test_err_clearing();
 test_droplink_safe_before_begin();
 test_blink_async_returns_immediately();
 test_blink_invalid_ignored();
 test_ishealthy_default();
 test_non_copyable();
 std::cout << "\n=== AutoLink Facade Tests Completed Successfully ===" << std::endl;
 return 0;
}
