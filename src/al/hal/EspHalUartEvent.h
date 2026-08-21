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
    // AL89-7: epoch captured when the current
    // window opened. If `breakWindowEpoch_`
    // (HAL) has advanced by the time the
    // window is summarized, the window
    // straddled a lock transition and the
    // pre-lock BREAKs would be (mis)counted
    // against the fresh OK session. Discard
    // the window in that case.
    uint32_t window_epoch_at_open = 0;

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
                // AL92-2: taskYIELD() switches to
                // the highest-priority READY task —
                // it can never schedule a LOWER-
                // priority task like loopTask
                // (prio 1) away from
                // uart_event_task (prio 5), so the
                // AL90-5 yield placed here bounded
                // nothing on the single-core
                // fallback it was written for. On
                // that fallback (singleCoreEvTask_,
                // set in begin() when the task
                // launches un-pinned), block with
                // vTaskDelay(1) instead — a true
                // yield that lets loopTask run —
                // but only once every
                // kSingleCoreYieldEvery onRx
                // events, so a sustained high-baud
                // stream still gets the event task's
                // attention most of the time rather
                // than paying a scheduler-tick delay
                // per byte burst. Dual-core boards
                // (the pinned, common case) keep the
                // plain taskYIELD() — cooperative
                // politeness among same-core peers,
                // not a starvation bound; the real
                // bound there is the core pin
                // itself (AL89-3 / AL92-1). Pinned
                // by UartEvTaskYieldsAfterOnRxTest.
                if (hal->singleCoreEvTask_) {
                    static constexpr uint32_t kSingleCoreYieldEvery = 8;
                    static uint32_t sSingleCoreYieldCounter = 0;
                    if ((++sSingleCoreYieldCounter %
                         kSingleCoreYieldEvery) == 0)
                        vTaskDelay(1);
                    else
                        taskYIELD();
                } else {
                    taskYIELD();
                }
            } else if (event.type == UART_FIFO_OVF ||
                       event.type == UART_BUFFER_FULL) {
                // RX overflow: the queue or the RX FIFO filled
                // up while the event task was blocked. The
                // default IDF path silently drops; a total RX
                // blackout produced no log line at all (defect
                // 1). Flush the input so the next packet is
                // not frame-errored against the partial
                // remainder, drain the event queue (one of
                // these events is usually followed by 4-6
                // more in the same millisecond), and log a
                // warning. Pinned by
                // EspHalUartOverflowCountersTest.
                hal->rxOverflows_++;
                uart_flush_input(hal->uart_num);
                xQueueReset(hal->uart_queue);
                Log::log().warning(TAG,
                                   "RX overflow (%s) — flushed %u bytes, "
                                   "queue reset (total overflows=%llu)",
                                   event.type == UART_FIFO_OVF ? "FIFO_OVF"
                                                               : "BUFFER_FULL",
                                   (unsigned)event.size,
                                   (unsigned long long)hal->rxOverflows_);
            } else if (event.type == UART_PARITY_ERR ||
                       event.type == UART_FRAME_ERR) {
                // Framing error. Distinct counter from
                // overflows (the link layer's frameErrs
                // counter is CRC-fail on a CRC-valid
                // packet, the wire-level "I saw a stop
                // bit in the wrong place" signal is
                // different). Surface it so a noisy
                // wire isn't silently counted as a
                // link-layer issue. Pinned by
                // EspHalUartOverflowCountersTest.
                hal->rxFrameErrs_++;
                Log::log().warning(TAG, "RX framing error (%s, total=%llu)",
                                   event.type == UART_PARITY_ERR ? "PARITY"
                                                                 : "FRAME",
                                   (unsigned long long)hal->rxFrameErrs_);
            } else if (event.type == UART_BREAK) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                // AL89-7: stamp the current BREAK-storm
                // window epoch the first time a BREAK
                // fires into an empty window. The
                // summary block below re-reads it and
                // discards the window if the epoch has
                // since advanced (a fresh lock happened
                // inside the window). Captured here
                // (not in the summary block) so a
                // window that started before a lock
                // can't fake being a window that
                // started after it.
                if (suppressed_count_window == 0 && delivered_count_window == 0)
                    window_epoch_at_open = hal->breakWindowEpoch_;

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
                // No RX flush here: this hook fires on every
                // DELIVERED BREAK, including an unconfirmed first
                // one and a coalesced duplicate the two-frame-clear
                // later resolves as healthy — flushing on any of
                // those destroys in-flight RX (OK state) or
                // in-flight sweep frames (SWP, where onBreak() is a
                // no-op) for a glitch that isn't a real drop. The
                // flush lives at the link layer's BREAK-confirm
                // sites instead (IHal::flushRxHw()). okState below
                // is unrelated (breakWindowEpoch_ tracking). Pinned
                // by EspHalBreakFlushOnConfirmOnlyTest.
                if (hal->events()) {
                    hal->events()->onBreak();
                }
            }
            // Window summary + storm escalation.
            uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(nowMs - last_break_summary_ms) >= BREAK_SUMMARY_MS) {
                // AL89-7: discard windows that
                // straddled a lock transition. The
                // pre-lock BREAKs would otherwise
                // inflate the post-lock session's
                // storm summary (the field capture
                // counted 263 pre-lock BREAKs in the
                // 263-BREAK storm ending 263 ms after
                // relock, every one of which was
                // already absorbed in pre-lock
                // arbitration). The same fair-chance
                // horizon already applied to
                // locksWithoutRecv_.
                bool windowStraddledLock =
                    (window_epoch_at_open != hal->breakWindowEpoch_);
                if (!windowStraddledLock &&
                    (suppressed_count_window > 0 ||
                     delivered_count_window > 0)) {
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
