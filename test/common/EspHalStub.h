// Host-test stubs for the ESP HAL types.
//
// include/AutoLink.h references EspHal / EspBlinkHal by name
// in the public constructor and in blinkHal/blinker members,
// but on the host we don't link src/al/hal/EspHal.cpp or any
// of FreeRTOS. The earlier header-based stub kept the public
// facade buildable for tests; this file is the new home so
// the production HAL boundary isn't blurred.
//
// Include only from test code under test/test_desktop/ or
// test/itest/test_desktop/. Do NOT include from src/ or
// include/ headers.
#pragma once
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see test/test_desktop/Makefile)"
#endif

#include "al/hal/IHal.h"

namespace autolink {
struct EspHal : public IHal {
    ~EspHal() override = default;
    EspHal() = default;
    EspHal(int, int, int, autolink::AutoLinkConfig) {}
    void begin() override {}
    void setSpd(uint32_t) override {}
    void sendBreak() override {}
    int tx(const uint8_t *, int) override { return 0; }
    void flushTx() override {}
    void startTimer(int) override {}
    void stopTimer() override {}
    void delayMs(int) override {}
    uint32_t nowMs() override { return 0; }
    void lock() override {}
    void unlock() override {}
    int pushAppBuf(const uint8_t *, int) override { return 0; }
    int popAppBuf(uint8_t *, int) override { return 0; }
    int peekAppBuf() const override { return -1; }
    int peekAt(uint8_t *, int, int) const override { return 0; }
    int appBufAvailable() const override { return 0; }
    void clearAppBuf() override {}
    void flushRxHw() override {}
};
struct EspBlinkHal {
    ~EspBlinkHal() = default;
    EspBlinkHal() = default;
    EspBlinkHal(int) {}
    void bind(void *) {}
};
} // namespace autolink