
#pragma once
#include "al/AutoLinkConfig.h"
#include "al/hal/IHal.h"
#include "al/util/log/Log.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "rom/ets_sys.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

namespace autolink {
class EspHal : public IHal {
    static constexpr const char *TAG = "EspHal";

    uart_port_t uart_num;
    int rx_pin;
    int tx_pin;
    AutoLinkConfig cfg;

    uint32_t last_setspd_ms = 0;

    QueueHandle_t uart_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    TimerHandle_t timer_handle = nullptr;
    StreamBufferHandle_t stream_buf = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t task_exit_sem = nullptr;
    volatile bool running = false;
    // Mirrors Link's OK/SWP state for the UART event task (see
    // EspHalUartEvent.h's UART_BREAK handling) — that task runs
    // outside Link's lock and can't read Link::state directly.
    // Set via setOkState() from Link::changeState_unlocked.
    volatile bool okState = false;
    // AL89-7: BREAK-storm window epoch. Bumped on every
    // 0->1 (SWP->OK) transition in setOkState. The UART
    // event task's suppressed/delivered window counters
    // are free-running and otherwise span an OK
    // transition — pre-lock BREAKs that would have
    // been silently absorbed (or, in the field capture,
    // escalated to a 263-BREAK storm 53 ms after
    // relock) inflate a healthy session's
    // "BREAK storm" warning. Captured at window-open
    // and re-checked at summary time; mismatch means
    // the window straddled a lock and is discarded.
    volatile uint32_t breakWindowEpoch_ = 0;
    bool healthy = false;
    // RX overflow / framing error counters. See
    // EspHalUartEvent.h's UART_FIFO_OVF /
    // UART_BUFFER_FULL / UART_PARITY_ERR /
    // UART_FRAME_ERR branches. A total RX blackout
    // produced no log line at all before these were
    // wired (defect 1 from the field-log
    // analysis). Pinned by
    // EspHalUartOverflowCountersTest.
    volatile uint64_t rxOverflows_ = 0;
    volatile uint64_t rxFrameErrs_ = 0;

    // AL92-2: set true in begin() exactly when
    // uart_event_task was launched un-pinned
    // (single-core fallback — see the evCore
    // discussion in begin()). taskYIELD() alone
    // cannot bound starvation there: it switches
    // to the highest-priority READY task, and
    // uart_event_task (priority 5) outranks
    // loopTask (priority 1), so a same-priority
    // yield never actually schedules loopTask.
    // The event task throttles itself with an
    // occasional blocking vTaskDelay(1) instead,
    // gated on this flag so dual-core boards (the
    // common case, where the pin already isolates
    // the two tasks onto separate cores) pay
    // nothing for it.
    bool singleCoreEvTask_ = false;

    size_t stream_buf_size_ = 0;

    size_t rx_buffer_size_ = 0;
    size_t tx_buffer_size_ = 0;

    static constexpr int PEEK_BUF_CAP = 16;
    mutable uint8_t peek_buf_[PEEK_BUF_CAP];
    mutable int peek_buf_len_ = 0;
    mutable int peek_buf_pos_ = 0;
    mutable bool appBufNullLogged_ = false;

    uart_config_t uart_config;


    static void uart_event_task(void *pvParameters);

    static void timer_callback(TimerHandle_t xTimer) {
        EspHal *hal = (EspHal *)pvTimerGetTimerID(xTimer);
        if (hal && hal->events())
            hal->events()->onTimer();
    }

public:
    EspHal(uart_port_t u_num, int rx_pin, int tx_pin,
           const AutoLinkConfig &config)
        : uart_num(u_num), rx_pin(rx_pin), tx_pin(tx_pin), cfg(config) {
        memset(&uart_config, 0, sizeof(uart_config_t));
        uart_config.baud_rate = 9600;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;
    }

    static size_t streamBufferFloor(const AutoLinkConfig &cfg) {
        return ::autolink::streamBufferFloor(cfg);
    }

    static size_t rxBufferFloor(const AutoLinkConfig &cfg) {
        return ::autolink::uartRxBufferFloor(cfg);
    }
    static size_t txBufferFloor(const AutoLinkConfig &cfg) {
        return ::autolink::uartTxBufferFloorCapped(
            cfg, ::esp_get_free_heap_size(), cfg.heapReserveBytes);
    }

    void setMode(AutoLinkConfig::Mode m) override {
        if (running) {
            cfg.mode = m;
            return;
        }
        cfg.mode = m;
        rx_buffer_size_ = rxBufferFloor(cfg);
        tx_buffer_size_ = txBufferFloor(cfg);
        stream_buf_size_ = streamBufferFloor(cfg);
    }
    AutoLinkConfig::Mode getMode() const override { return cfg.mode; }

    void begin() override;
    void begin(const AutoLinkConfig &c) override {
        if (!running) {
            // Adopt the whole config, not just the mode. setMode() alone
            // left cfg.maxMsg stale at whatever EspHal's constructor saw
            // (a compile-time copy, taken before AutoLinkWeb exists to
            // cap it) — the buffer floors were then sized for the wrong
            // maxMsg regardless of what AutoLink::setMaxMsg() installed
            // on the Link's own config. Assign first so setMode()'s
            // floor recompute below sees the caller's real maxMsg.
            cfg = c;
            setMode(c.mode);
        }
        begin();
    }
    ~EspHal();

    bool isHealthy() const override { return healthy; }

    // The installed size, not the config floor: Link::begin() gates on it
    // and treats 0 as "unknown". Pinned by BeginRejectsHeapClampedRingTest.
    size_t txRingSize() const override { return tx_buffer_size_; }
    size_t rxRingSize() const override { return stream_buf_size_; }

    void setSpd(uint32_t spd) override {
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(20));

        uart_flush_input(uart_num);
        esp_err_t e = uart_set_baudrate(uart_num, spd);
        if (e != ESP_OK)
            Log::log().error(TAG, "set_baudrate(%lu): %s", (unsigned long)spd,
                             esp_err_to_name(e));
        else
            Log::log().debug(TAG, "set_baudrate %lu OK (BREAK guard arm)",
                             (unsigned long)spd);

        last_setspd_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }

    void sendBreak() override {
        uart_write_bytes_with_break(uart_num, " ", 1, 15);
    }

    int tx(const uint8_t *b, int n) override {
        // Measure the block time so a single tx() that holds
        // the link lock for longer than one OK-tick gets
        // surfaced — defect 1 in the field-log
        // analysis: blocking under the link lock is the
        // exact failure mode the bounded burst is meant to
        // prevent; the timer check here surfaces the
        // regression. Pinned by
        // EspHalTxBlockedMsTest.
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        int w = uart_write_bytes(uart_num, (const char *)b, n);
        uint32_t dt = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        if (dt > 50)
            Log::log().warning(TAG, "tx blocked %lu ms (n=%d) — lock held",
                               (unsigned long)dt, n);
        // Forward the per-call blocked time to the link layer
        // so Stats::txBlockedMs reflects reality. Earlier
        // path kept dt local and the aggregate was permanently
        // zero regardless of the wire's contention. Forwarded
        // unconditionally (not gated on dt>0) so a fast
        // uart_write_bytes still contributes — the field log
        // shows real frames completing in <1 ms when the ring
        // is drained, and those add up across a 5 s stats
        // window. events() is the ILinkEvents* the link
        // registered via setEvents; calling it routes the
        // accumulate into the link's txBlockedMs_ under the
        // link's own lock. Pinned by StatsTxBlockedMsTest.
        if (events())
            events()->onTxBlockedNote(dt);
        // Per-frame tx accounting: errored frames never reach
        // the link layer's txBytes counter, so the operator
        // sees a stalled tx counter on a noisy wire. Debug,
        // not info, because per-frame is the ASYNC pipeline
        // rate — info would flood.
        if (w < n)
            Log::log().warning(TAG, "tx truncated: asked=%d wrote=%d", n, w);
        return w;
    }

    int txAvail() const override {
        // uart_get_tx_buffer_free_size() returns the free-byte count
        // directly on older IDF; IDF 5.x changed it to esp_err_t plus an
        // out-param, matching the rest of the driver's error-reporting
        // convention. A driver-level failure (unlikely once begin() has
        // succeeded) reports 0 free rather than propagating an error
        // through an int-returning interface.
        size_t freeBytes = 0;
        esp_err_t e = uart_get_tx_buffer_free_size(uart_num, &freeBytes);
        return e == ESP_OK ? (int)freeBytes : 0;
    }
    uint64_t rxOverflowCount() const override { return rxOverflows_; }
    uint64_t rxFrameErrCount() const override { return rxFrameErrs_; }

    void flushTx() override { uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100)); }

    // No IDF TX-ring purge exists; capped drain at the current baud.
    void discardTx() override {
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(20));
    }

    void startTimer(int ms) override {
        if (!timer_handle)
            return;
        const TickType_t blk = pdMS_TO_TICKS(20);
        const TickType_t per = pdMS_TO_TICKS(ms);
        for (int i = 0; i < 3; i++) {
            if (xTimerChangePeriod(timer_handle, per, blk) == pdPASS &&
                xTimerStart(timer_handle, blk) == pdPASS)
                return;
        }
        Log::log().error(TAG,
                         "startTimer(%dms) failed:"
                         " queue full",
                         ms);
    }

    void stopTimer() override {
        if (timer_handle)
            xTimerStop(timer_handle, pdMS_TO_TICKS(20));
    }

    void delayMs(int ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }

    // Busy-wait, not vTaskDelay: a ms-resolution sleep rounds up to
    // the FreeRTOS tick (10 ms @ 100 Hz) and would 10x the ASYNC
    // inter-chunk gap. Pinned by AsyncChunkGapTest.
    void delayUs(uint32_t us) override { ets_delay_us(us); }

    uint32_t nowMs() override {
#ifdef ARDUINO
        return (uint32_t)millis();
#else
        return (uint32_t)(esp_timer_get_time() / 1000);
#endif
    }

    void lock() override {
        if (mutex)
            xSemaphoreTake(mutex, portMAX_DELAY);
    }

    void unlock() override {
        if (mutex)
            xSemaphoreGive(mutex);
    }

    bool tryLock(int timeoutMs) override {
        if (!mutex)
            return true;
        return xSemaphoreTake(mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    int pushAppBuf(const uint8_t *b, int n) override {
        if (n <= 0)
            return 0;
        if (!stream_buf) {
            if (!appBufNullLogged_) {
                Log::log().error(TAG,
                                 "pushAppBuf: "
                                 "stream_buf NULL — "
                                 "all RX dropped");
                appBufNullLogged_ = true;
            }
            return 0;
        }
        return (int)xStreamBufferSend(stream_buf, b, n, 0);
    }

    int popAppBuf(uint8_t *b, int max_len) override {
        int total = 0;
        while (total < max_len && peek_buf_pos_ < peek_buf_len_)
            b[total++] = peek_buf_[peek_buf_pos_++];
        if (peek_buf_pos_ >= peek_buf_len_) {
            peek_buf_len_ = 0;
            peek_buf_pos_ = 0;
        }
        if (total < max_len && stream_buf) {
            size_t r =
                xStreamBufferReceive(stream_buf, b + total, max_len - total, 0);
            total += (int)r;
        }
        return total;
    }

    int peekAppBuf() const override {
        if (peek_buf_pos_ < peek_buf_len_)
            return peek_buf_[peek_buf_pos_];
        if (stream_buf) {
            uint8_t b;
            if (xStreamBufferReceive(stream_buf, &b, 1, 0) == 1 &&
                peek_buf_len_ < PEEK_BUF_CAP) {
                peek_buf_[peek_buf_len_++] = b;
                return peek_buf_[peek_buf_pos_];
            }
        }
        return -1;
    }

    int peekAt(uint8_t *out, int n, int offset) const override {
        if (n <= 0 || offset < 0)
            return 0;
        int copied = 0, pos = offset;
        if (pos < peek_buf_len_) {
            int from = std::min(n, peek_buf_len_ - pos);
            memcpy(out, peek_buf_ + pos, from);
            copied += from;
            pos = 0;
        } else {
            pos -= peek_buf_len_;
        }
        while (copied < n) {
            uint8_t tmp;
            if (!stream_buf)
                break;
            if (xStreamBufferReceive(stream_buf, &tmp, 1, 0) != 1)
                break;
            out[copied++] = tmp;
            if (peek_buf_len_ < PEEK_BUF_CAP)
                peek_buf_[peek_buf_len_++] = tmp;
        }
        return copied;
    }

    int appBufAvailable() const override {
        int n = stream_buf ? (int)xStreamBufferBytesAvailable(stream_buf) : 0;
        n += peek_buf_len_ - peek_buf_pos_;
        return n;
    }

    int appBufFree() const override {
        // stream_buf is a FreeRTOS StreamBufferHandle_t;
        // xStreamBufferSpacesAvailable reports the
        // number of bytes the buffer will accept on the
        // next xStreamBufferSend. The peek buffer is a
        // separate pre-decode stash; it doesn't grow
        // during a single onPayload call so it's not
        // counted here.
        if (!stream_buf)
            return INT32_MAX;
        size_t free_ = xStreamBufferSpacesAvailable(stream_buf);
        // The FreeRTOS StreamBuffer has the +1
        // quirk documented in the header; the "usable
        // capacity" is one less than the size we
        // passed to xStreamBufferCreate. Don't admit
        // the boundary byte.
        if (free_ > 0)
            return (int)(free_ - 1);
        return 0;
    }

    void clearAppBuf() override {
        if (stream_buf)
            xStreamBufferReset(stream_buf);
        peek_buf_len_ = 0;
        peek_buf_pos_ = 0;
    }

    void flushRxHw() override { uart_flush_input(uart_num); }
    void setOkState(bool ok) override {
        if (ok && !okState)
            breakWindowEpoch_++;
        okState = ok;
    }
};

} // namespace autolink
