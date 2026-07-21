// UART event task — split out of EspHal.h to keep
// the per-file 15 KB cap. The task body lives here
// (inline in the header to avoid a separate .cpp
// translation unit); EspHal.h owns the task's
// declaration and the launch / join code.
#pragma once
#include <Arduino.h>
#include "EspHal.h"

namespace autolink {

inline void EspHal::uart_event_task(void *pvParameters) {
    EspHal *hal = (EspHal *)pvParameters;
    hal->running = true;
    Log::log().info(TAG, "uart_event_task core=%d", xPortGetCoreID());

    uart_event_t event;
    size_t rx_cap = hal->stream_buf_size_;
    uint8_t *rx_buf = (uint8_t *)malloc(rx_cap);
    if (!rx_buf) {
        Log::log().error(TAG, "uart_event_task: rx_buf alloc failed");
        vTaskDelete(NULL);
        return;
    }

    // BREAK storm tracking: count suppressed vs
    // delivered, log the summary once per
    // BREAK_SUMMARY_MS, and escalate a sustained
    // count above BREAK_STORM_THRESHOLD as a
    // baud-mismatch signal. Pinned by BreakStormSummaryTest.
    constexpr uint32_t BREAK_SUMMARY_MS = 1000;
    constexpr uint32_t BREAK_STORM_THRESHOLD = 8;
    constexpr uint32_t BREAK_DEBOUNCE_MS = 120;
    constexpr uint32_t POST_SETSPD_BREAK_GUARD_MS = 80;
    uint32_t last_break_ms = 0;
    uint32_t last_break_summary_ms = 0;
    uint32_t suppressed_count_window = 0;
    uint32_t delivered_count_window = 0;
    uint8_t storm_announced = 0;

    while (hal->running) {
        if (xQueueReceive(hal->uart_queue, (void *)&event,
                          pdMS_TO_TICKS(100))) {
            if (!hal->running)
                break;
            if (event.type == UART_DATA) {
                size_t rlen = (event.size > rx_cap) ? rx_cap : event.size;
                int len =
                    uart_read_bytes(hal->uart_num, rx_buf, rlen, portMAX_DELAY);
                if (hal->events() && len > 0)
                    hal->events()->onRx(rx_buf, len);
            } else if (event.type == UART_BREAK) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

                if ((uint32_t)(now - hal->last_setspd_ms) <
                    POST_SETSPD_BREAK_GUARD_MS) {
                    suppressed_count_window++;
                    last_break_ms = now;
                    continue;
                }
                if ((uint32_t)(now - last_break_ms) < BREAK_DEBOUNCE_MS) {
                    suppressed_count_window++;
                    continue;
                }
                last_break_ms = now;
                delivered_count_window++;
                uart_flush_input(hal->uart_num);
                if (hal->events()) {
                    hal->events()->onBreak();
                }
            }
            // Window summary + storm escalation.
            uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(nowMs - last_break_summary_ms) >= BREAK_SUMMARY_MS) {
                if (suppressed_count_window > 0 || delivered_count_window > 0) {
                    if (suppressed_count_window >= BREAK_STORM_THRESHOLD) {
                        Log::log().warning(
                            TAG,
                            "BREAK storm: %lu suppressed, %lu "
                            "delivered in last %d ms (likely "
                            "baud mismatch — recommend "
                            "P1 walk)",
                            (unsigned long)suppressed_count_window,
                            (unsigned long)delivered_count_window,
                            (int)BREAK_SUMMARY_MS);
                        if (!storm_announced) {
                            storm_announced = 1;
                            if (hal->events()) {
                                hal->events()->onBreakStorm();
                            }
                        }
                    } else if (suppressed_count_window > 0 ||
                               delivered_count_window > 0) {
                        Log::log().debug(TAG,
                                         "BREAK summary: %lu suppressed, %lu "
                                         "delivered in last %d ms",
                                         (unsigned long)suppressed_count_window,
                                         (unsigned long)delivered_count_window,
                                         (int)BREAK_SUMMARY_MS);
                    }
                }
                suppressed_count_window = 0;
                delivered_count_window = 0;
                last_break_summary_ms = nowMs;
                // Re-arm the storm hook for the next
                // window: without this clear the flag
                // latches for the process lifetime and a
                // second storm later in the session
                // produces no escalation.
                storm_announced = 0;
            }
        }
    }
    free(rx_buf);
    if (hal->task_exit_sem)
        xSemaphoreGive(hal->task_exit_sem);
    vTaskDelete(NULL);
}

} // namespace autolink
