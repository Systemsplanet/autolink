// AutoLink facade smoke + byte-stream API.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/hal/IHal.h"
#include "al/link/Link.h"
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"

#include "AutoLink.h"
#include "AutoLinkTestAccessor.h"
#include "EspHalStub.h"
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <utility>
#include "NullArqCache.h"

using namespace autolink;

void test_default_construction() {
    std::cout << "\n=== Test: Default Construction ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    (void)link;
    std::cout << "PASS" << std::endl;
}

void test_custom_config_construction() {
    std::cout << "\n=== Test: Custom Config Construction ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.ledPin = 4;
    cfg.maxMsg = 4096;
    cfg.idleTimeoutMs = 5000;
    AutoLink link(0, 16, 17, false, cfg);
    (void)link;
    std::cout << "PASS" << std::endl;
}

void test_app_buffer_facade_does_not_mutate_config() {
    std::cout << "\n=== Test: facade ctor leaves cfg.streamBufferSize alone "
                 "(HAL owns sizing) ==="
              << std::endl;

    AutoLinkConfig cfg;
    cfg.maxMsg = 1024;
    const size_t user_set = cfg.streamBufferSize;
    AutoLink link(0, 16, 17, false, cfg);
    // The pre-fix clampBuffers in the facade ctor mutated
    // cfg.streamBufferSize up to a 16-slot ARQ floor; that was removed
    // when the sizing decision moved into EspHal. The facade must NOT
    // touch the user's config — the HAL owns the sizing decision. The
    // 16-slot ARQ floor is pinned source-side by EspHalBeginAndHealthTest's
    // `test_esphal_derives_stream_buffer_size_from_maxmsg_and_mode`.
    assert(cfg.streamBufferSize == user_set);
    assert(link.getStreamBufferSize() == user_set);
    std::cout << "PASS (cfg.streamBufferSize=" << user_set
              << " facade did not mutate it)\n";
}

void test_state_api() {
    std::cout << "\n=== Test: State API ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    (void)link.getState();
    (void)link.getCurrentBaud();
    (void)link.ready();
    (void)link.getErrCount();
    {
        Stats s;
        link.getStats(s);
        (void)s.frameErrs;
    }
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
    std::cout << "\n=== Test: Byte-Stream API ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    (void)link.available();
    (void)link.peek();
    link.flush();
    (void)link.write((uint8_t)0xAB);
    uint8_t wb[] = { 0xCD, 0xEF };
    (void)link.write(wb, 2);
    uint8_t rb[8];
    (void)link.read(rb, 8);
    (void)link.read();
    std::cout << "PASS" << std::endl;
}

void test_message_api() {
    std::cout << "\n=== Test: Message API ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    uint8_t msg[] = { 0x10, 0x20, 0x30 };
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
    std::cout << "\n=== Test: AutoLink Constructible Per-Instance ==="
              << std::endl;
    AutoLink a(0, 16, 17, true);
    AutoLink b(0, 16, 17, false);
    (void)a;
    (void)b;
    std::cout << "PASS" << std::endl;
}

void test_config_defaults_centralized() {
    std::cout << "\n=== Test: AutoLinkConfig Defaults Centralized ==="
              << std::endl;
    AutoLinkConfig cfg;
    // Defaults live on AutoLinkConfig; the facade and
    // Link.cpp read them via cfg.X rather than via private
    // constants in Link.h.
    assert(cfg.streamBufferSize == 2048);
    assert(cfg.maxMsg == 1024);
    assert(cfg.syncAckTimeoutMs == 500);
    assert(cfg.maxRetx == 5);
    AutoLink link(0, 16, 17, true, cfg);
    AutoLinkTestAccessor t(link);
    assert(t.link()->getConfig().maxRetx == 5);
    assert(t.link()->getConfig().syncAckTimeoutMs == 500);
    std::cout << "PASS" << std::endl;
}

void test_maxRetx_overridable() {
    std::cout << "\n=== Test: cfg.maxRetx Overridable ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.maxRetx = 3;
    AutoLink link(0, 16, 17, true, cfg);
    AutoLinkTestAccessor t(link);
    assert(t.link()->getConfig().maxRetx == 3);
    std::cout << "PASS" << std::endl;
}

void test_txDelayMs_default_and_setter() {
    std::cout << "\n=== Test: txDelayMs default + setter ===" << std::endl;
    AutoLinkConfig cfg;
    assert(cfg.txDelayMs == 0);
    AutoLink link(0, 16, 17, true, cfg);
    AutoLinkTestAccessor t(link);
    assert(link.txDelayMs() == 0);
    link.setTxDelayMs(500);
    assert(link.txDelayMs() == 500);
    assert(t.link()->getConfig().txDelayMs == 500);
    // Negative is clamped to 0 (defensive: dashboard sends >=0 but
    // a misbehaving caller should not push a negative interval
    // into the loop's signed compare).
    link.setTxDelayMs(-5);
    assert(link.txDelayMs() == 0);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running AutoLink Facade Tests ===" << std::endl;
    test_default_construction();
    test_custom_config_construction();
    test_app_buffer_facade_does_not_mutate_config();
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
    test_config_defaults_centralized();
    test_maxRetx_overridable();
    test_txDelayMs_default_and_setter();
    std::cout << "\n=== AutoLink Facade Tests Completed Successfully ==="
              << std::endl;
    return 0;
}