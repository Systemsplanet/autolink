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
//
// LED defaults to GPIO 2 (the onboard blue LED on most ESP32 dev boards).
// Point it elsewhere with cfg.ledPin; tune timing with blinkWait(n, onMs, offMs).
//
// Two modes (selected by the delayMs argument to AutoLink::blinkWait):
//   Async  (delayMs == 0, default): returns immediately; the pattern runs on
//          an esp_timer in the background. A new call replaces any running
//          pattern. Use this for per-packet heartbeats — zero loop overhead.
//   Blocking (delayMs > 0): flashes n times then pauses delayMs ms, holding
//          loop() for n*(onMs+offMs)+delayMs. The UART keeps buffering while
//          you wait, but your code won't drain it until the call returns.
//
// Suggested sketch convention:
//   blinkWait(3, 100, 100, 2000)  — repeated while searching / negotiating baud
//   blinkWait(4)                  — one burst on first connect
//   blinkWait(1)                  — one flash per packet sent or echoed
// ----------------------------------------------------------------------------

namespace autolink {

// ----------------------------------------------------------------------------
// IBlinkHal — hardware seam for UtilBlink.
//
// Abstracts the three things the blink engine needs from hardware: a GPIO
// pin to toggle, a one-shot timer to schedule the next phase, and a blocking
// delay for the synchronous path. EspBlinkHal is the ESP32 implementation;
// mock implementations drive the host unit tests.
// ----------------------------------------------------------------------------
class IBlinkHal {
public:
    virtual ~IBlinkHal() {}
    virtual void writePin(bool on) = 0;
    virtual void startOnce(uint32_t ms) = 0;  // schedule one tick() callback
    virtual void cancel() = 0;                // stop a pending tick
    virtual void delayMs(uint32_t ms) = 0;
    // Cooperative yield to the scheduler. Used by flashBlocking() after
    // cancel() to drain any in-flight esp_timer callback that esp_timer_stop()
    // (which is non-blocking) couldn't wait for. No-op on host; the ESP
    // implementation calls portYIELD() so the timer task can finish
    // dispatching the previous pattern's callback before the blocking loop
    // starts toggling the pin.
    virtual void yield() {}
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
        // esp_timer_stop() is non-blocking: per ESP-IDF docs, "after
        // esp_timer_stop() the timer is disarmed, but its callback may
        // still be running." If the previous async pattern's callback
        // (cb -> tick()) was mid-dispatch on the timer task when we
        // entered cancel(), it could re-arm the timer via startOnce()
        // while our for-loop is calling delay() — and the volatile
        // state shared with the callback is in flux for a few
        // microseconds. hal.yield() lets any in-flight callback run to
        // completion (it will see left=0 / on=false and not re-arm)
        // before we start toggling the pin. This is the single line
        // that fixes the "hangs at blinkWait(2,50,50,100) right after
        // a blinkWait(1,60,60,0)" symptom observed in
        // test_embedded.ino: the 60ms one-shot fires during the next
        // test's Serial.printf and the callback races with the
        // blocking cancel() if we don't yield here. No-op on host.
        hal.yield();
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

// ----------------------------------------------------------------------------
// EspBlinkHal — ESP32 implementation of IBlinkHal.
//
// Drives a GPIO pin via digitalWrite and schedules blink ticks with an
// esp_timer one-shot. Constructed by AutoLink; bind() wires it to the
// UtilBlink instance so the timer callback can call tick().
// ----------------------------------------------------------------------------
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
    void yield() override {
        // portYIELD() produces a single yield-from-task switch. The timer
        // task is normally lower priority than the calling task on ESP-IDF,
        // so this lets it run one tick and dispatch any in-flight callback.
        // Safe to call repeatedly; the cost is a few microseconds.
        portYIELD();
    }
};

#endif // ARDUINO || ESP_PLATFORM

} // namespace autolink
