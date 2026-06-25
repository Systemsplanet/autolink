// LED blink pattern driver.
// IBlinkHal abstracts pin + one-shot timer.
// EspBlinkHal implements with esp_timer.
#pragma once
#include <stdint.h>
#ifdef ARDUINO
#    include <freertos/FreeRTOS.h>
#endif

namespace autolink
{
class IBlinkHal
{
public:
    virtual ~IBlinkHal() {}
    virtual void writePin(bool on) = 0;
    virtual void startOnce(uint32_t ms) = 0;
    virtual void cancel() = 0;
    virtual void delayMs(uint32_t ms) = 0;

    virtual void yield() {}
};

class UtilBlink
{
    IBlinkHal &hal;
    volatile int left = 0;
    volatile bool on = false;
    int onMs = 60, offMs = 60;

public:
    explicit UtilBlink(IBlinkHal &h) : hal(h) {}

    void start(int n, int onTimeMs = 60, int offTimeMs = 60)
    {
        if (n <= 0)
            return;
        cancel();
        onMs = onTimeMs;
        offMs = offTimeMs;
        left = n;
        tick();
    }

    void flashBlocking(int n, int onTimeMs, int offTimeMs, long delayMs)
    {
        cancel();

        hal.yield();
        for (int i = 0; i < n; i++) {
            hal.writePin(true);
            hal.delayMs((uint32_t)onTimeMs);
            hal.writePin(false);
            if (i < n - 1)
                hal.delayMs((uint32_t)offTimeMs);
        }
        if (delayMs > 0)
            hal.delayMs((uint32_t)delayMs);
    }

    void cancel()
    {
        hal.cancel();
        left = 0;
        on = false;
        hal.writePin(false);
    }

    void tick()
    {
        if (on) {
            hal.writePin(false);
            on = false;
            if (left > 0)
                hal.startOnce((uint32_t)offMs);
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
}

#    include <Arduino.h>

#    if defined(ARDUINO) && !defined(ESP_IDF_VERSION) && \
        !defined(AUTOLINK_USE_ESP_TIMER)
#        include "al/util/UtilBlinkEspTimerShim.h"
#    else
#        include "esp_timer.h"
#    endif

namespace autolink
{
class EspBlinkHal : public IBlinkHal
{
    int pin;
    esp_timer_handle_t timer = nullptr;
    UtilBlink *owner = nullptr;

    static void cb(void *arg)
    {
        EspBlinkHal *self = (EspBlinkHal *)arg;
        if (self->owner)
            self->owner->tick();
    }
    void ensureTimer()
    {
        if (timer)
            return;
        esp_timer_create_args_t a = {};
        a.callback = &EspBlinkHal::cb;
        a.arg = this;
        a.name = "alink_blink";
        esp_timer_create(&a, &timer);
    }

public:
    explicit EspBlinkHal(int ledPin) : pin(ledPin)
    {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
    ~EspBlinkHal()
    {
        if (timer) {
            esp_timer_stop(timer);
            esp_timer_delete(timer);
        }
    }
    void bind(UtilBlink *o) { owner = o; }

    void writePin(bool on) override { digitalWrite(pin, on ? HIGH : LOW); }
    void startOnce(uint32_t ms) override
    {
        ensureTimer();
        esp_timer_stop(timer);
        esp_timer_start_once(timer, (uint64_t)ms * 1000);
    }
    void cancel() override
    {
        if (timer)
            esp_timer_stop(timer);
    }
    void delayMs(uint32_t ms) override { delay(ms); }
    void yield() override { portYIELD(); }
};

#endif
}