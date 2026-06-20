// UtilBlinkEspTimerShim.h — minimal no-op stub of <esp_timer.h> for the
// Arduino-core build path.
//
// ESP-IDF ships <esp_timer.h> at
// $IDF_PATH/components/esp_timer/include/esp_timer.h, and the project
// includes it from UtilBlink.h for the async-blink timer. The Arduino
// core (especially ArduinoDroid, older Arduino cores) does NOT include
// ESP-IDF, so <esp_timer.h> doesn't exist in that include path.
//
// When the build is Arduino (ARDUINO defined) AND not ESP-IDF
// (ESP_IDF_VERSION undefined) AND the user hasn't asked for the real
// timer (AUTOLINK_USE_ESP_TIMER undefined), UtilBlink.h includes this
// shim instead. The shim provides the symbols EspBlinkHal needs
// (esp_timer_create, esp_timer_start_once, esp_timer_stop,
// esp_timer_delete, the esp_timer_handle_t type, and
// esp_timer_create_args_t) as no-op inline stubs. The async-blink
// timer doesn't actually fire (the shimmed create returns a fake
// handle that start_once / stop ignore), so the LED will not blink
// asynchronously. Blocking blinkWait() still works because that's
// just delay() in a loop — it doesn't use the timer.
//
// The shim is intentionally minimal. If you need real async blink on
// Arduino, install the esp_timer component manually (copy the four
// .c/.h files into your project) and define AUTOLINK_USE_ESP_TIMER
// before including AutoLink.h. Or just use the blocking path via
// blinkWait(n, on, off, delayMs) which is the default in the bundled
// examples anyway.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void*);
#define ESP_TIMER_TASK 0

struct esp_timer_create_args_t {
    esp_timer_cb_t callback;
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};

inline int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* out) {
    if (out) *out = reinterpret_cast<esp_timer_handle_t>(0x1);  // fake non-null handle
    return 0;
}
inline int esp_timer_start_once(esp_timer_handle_t, uint64_t) { return 0; }
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return 0; }
inline int esp_timer_stop(esp_timer_handle_t) { return 0; }
inline int esp_timer_delete(esp_timer_handle_t) { return 0; }
inline long long esp_timer_get_time() { return 0; }
