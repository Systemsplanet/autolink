// Host-side shim for the esp_timer_* API used by
// EspBlinkHal. Lets UtilBlink.h compile under the host
// test build without pulling in the real esp_timer
// driver.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);
#define ESP_TIMER_TASK 0

struct esp_timer_create_args_t {
    esp_timer_cb_t callback;
    void *arg;
    int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
};

inline int
esp_timer_create(const esp_timer_create_args_t *,
                 esp_timer_handle_t *out)
{
    if (out)
        *out =
            reinterpret_cast<esp_timer_handle_t>(0x1);
    return 0;
}
inline int esp_timer_start_once(esp_timer_handle_t,
                                uint64_t)
{
    return 0;
}
inline int esp_timer_start_periodic(esp_timer_handle_t,
                                    uint64_t)
{
    return 0;
}
inline int esp_timer_stop(esp_timer_handle_t)
{
    return 0;
}
inline int esp_timer_delete(esp_timer_handle_t)
{
    return 0;
}
inline long long esp_timer_get_time()
{
    return 0;
}