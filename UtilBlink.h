#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// UtilBlink — reusable LED flash-pattern engine, async or blocking.
//
// start(n) flashes a pin n times in the background: the engine schedules
// one-shot timer ticks through an injected IBlinkHal and returns immediately,
// so a per-packet heartbeat costs the caller nothing. flashBlocking() runs
// the same pattern inline plus a trailing pause, for pacing a loop. A new
// start() replaces any pattern still running. The HAL seam keeps the engine
// fully host-testable; EspBlinkHal below is the ESP32 implementation.
// ----------------------------------------------------------------------------

namespace autolink {

// Hardware seam for UtilBlink: a pin, a one-shot tick timer, and a delay.
class IBlinkHal {
public:
    virtual ~IBlinkHal() {}
    virtual void writePin(bool on) = 0;
    virtual void startOnce(uint32_t ms) = 0;  // schedule one tick() callback
    virtual void cancel() = 0;                // stop a pending tick
    virtual void delayMs(uint32_t ms) = 0;
};

class UtilBlink {
    IBlinkHal& hal;
    volatile int  left = 0;     // flashes not yet started
    volatile bool on   = false; // LED currently lit by the engine
    int onMs = 60, offMs = 60;

public:
    explicit UtilBlink(IBlinkHal& h) : hal(h) {}

    // Async: light up now, return immediately; the HAL's tick() drives the
    // rest. Replaces any running pattern.
    void start(int n, int onTimeMs = 60, int offTimeMs = 60) {
        if (n <= 0) return;
        cancel();
        onMs = onTimeMs;
        offMs = offTimeMs;
        left = n;
        tick();
    }

    // Blocking: flash n times inline, then pause delayMs.
    void flashBlocking(int n, int onTimeMs, int offTimeMs, long delayMs) {
        cancel();
        for (int i = 0; i < n; i++) {
            hal.writePin(true);
            hal.delayMs((uint32_t)onTimeMs);
            hal.writePin(false);
            if (i < n - 1) hal.delayMs((uint32_t)offTimeMs);
        }
        if (delayMs > 0) hal.delayMs((uint32_t)delayMs);
    }

    // Stop any running pattern and force the LED off.
    void cancel() {
        hal.cancel();
        left = 0;
        on = false;
        hal.writePin(false);
    }

    // Timer expiry callback: advances the on/off pattern one phase.
    void tick() {
        if (on) {
            hal.writePin(false);
            on = false;
            if (left > 0) hal.startOnce((uint32_t)offMs);
        } else if (left > 0) {
            left = left - 1;
            hal.writePin(true);
            on = true;
            hal.startOnce((uint32_t)onMs);
        }
    }

    bool active() const { return left > 0 || on; }
};

#if defined(ARDUINO) || defined(ESP_PLATFORM)
} // namespace autolink

#include <Arduino.h>
#include "esp_timer.h"

namespace autolink {

// ESP32 IBlinkHal: a GPIO pin plus an esp_timer for the async ticks.
class EspBlinkHal : public IBlinkHal {
    int pin;
    esp_timer_handle_t timer = nullptr;
    UtilBlink* owner = nullptr;

    static void cb(void* arg) {
        EspBlinkHal* self = (EspBlinkHal*)arg;
        if (self->owner) self->owner->tick();
    }
    void ensureTimer() {
        if (timer) return;
        esp_timer_create_args_t a = {};
        a.callback = &EspBlinkHal::cb;
        a.arg = this;
        a.name = "alink_blink";
        esp_timer_create(&a, &timer);
    }

public:
    explicit EspBlinkHal(int ledPin) : pin(ledPin) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
    ~EspBlinkHal() {
        if (timer) { esp_timer_stop(timer); esp_timer_delete(timer); }
    }
    void bind(UtilBlink* o) { owner = o; }

    void writePin(bool on) override { digitalWrite(pin, on ? HIGH : LOW); }
    void startOnce(uint32_t ms) override {
        ensureTimer();
        esp_timer_stop(timer);
        esp_timer_start_once(timer, (uint64_t)ms * 1000);
    }
    void cancel() override { if (timer) esp_timer_stop(timer); }
    void delayMs(uint32_t ms) override { delay(ms); }
};

#endif // ARDUINO || ESP_PLATFORM

} // namespace autolink
