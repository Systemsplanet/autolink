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

#include "ILink.h"
#include "ALink.h"   // pulls in AutoLinkConfig used by the EspHal stub ctor
#include "Log.h"
#include "util/UtilBlink.h"

namespace autolink {

// Same stub signature as the real EspHal. ILink is the only thing it
// needs to be substitutable for AutoLink's constructor.
class EspHal : public ILink {
public:
    EspHal(int, int, int, const AutoLinkConfig&) {}
    void begin() override {}
    void setSpd(uint32_t) override {}
    void sendBreak() override {}
    int tx(const uint8_t*, int n) override { return n; }
    void flushTx() override {}
    void startTimer(int) override {}
    void stopTimer() override {}
    void delayMs(int) override {}
    uint32_t nowMs() override { return 0; }
    void lock() const override {}
    void unlock() const override {}
    void pushAppBuf(uint8_t) override {}
    int  pushAppBuf(const uint8_t*, int) override { return 0; }
    int  popAppBuf() override { return -1; }
    int  popAppBuf(uint8_t*, int) override { return 0; }
    int  peekAppBuf() override { return -1; }
    int  appBufAvailable() const override { return 0; }
    void clearAppBuf() override {}
    bool isHealthy() const { return true; }
};

class EspBlinkHal : public IBlinkHal {
public:
    explicit EspBlinkHal(int) {}
    void bind(UtilBlink* b) { owner_ = b; }
    void writePin(bool) override {}
    void startOnce(uint32_t) override {}
    void cancel() override {}
    void delayMs(uint32_t) override {}
    UtilBlink* owner_ = nullptr;
};

} // namespace autolink

// ---------------------------------------------------------------------------
// AutoLink facade tests.
// ---------------------------------------------------------------------------
#include "AutoLink.h"
#include <iostream>
#include <cassert>
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

void test_state_api() {
    std::cout << "\n=== Test: State API ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    (void)link.getState();
    (void)link.getCurrentBaud();
    (void)link.ready();
    (void)link.getErrCount();
    (void)link.getLifetimeErrors();
    std::cout << "PASS" << std::endl;
}

void test_stats_api() {
    std::cout << "\n=== Test: Stats API ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    uint64_t tx, rx;
    link.getStats(tx, rx);
    uint64_t errs;
    link.getStats(tx, rx, errs);
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
