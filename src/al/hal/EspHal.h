// EspHal.h — ESP32 implementation of the ILink hardware abstraction.
//
// Owns the UART driver, FreeRTOS event task, software timer, mutex, and
// stream buffer. Constructed by AutoLink; never used directly in sketches.
// For host testing use MockHal (defined in test.cpp) instead.
#pragma once
#include "al/hal/ILink.h"
#include "al/protocol/ALink.h"
#include "al/util/Log.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <string.h> // memset
#include <stdlib.h>  // malloc/free

namespace autolink {

// ----------------------------------------------------------------------------
// EspHal — the ESP32 implementation of ILink: ESP-IDF UART driver with a
// dedicated FreeRTOS event task (data + BREAK with debounce), a FreeRTOS
// software timer for the protocol clock, a mutex for the protocol lock, and
// a stream buffer for the app-side bytes.
// ----------------------------------------------------------------------------
class EspHal : public ILink {
    static constexpr const char* HAL_TAG = "EspHal";

    uart_port_t uart_num;
    int rx_pin;
    int tx_pin;
    AutoLinkConfig cfg;
    
    QueueHandle_t uart_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    TimerHandle_t timer_handle = nullptr;
    StreamBufferHandle_t stream_buf = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t task_exit_sem = nullptr;
    volatile bool running = false;
    bool healthy = false;
    // One-byte look-ahead cache for peekAppBuf(). Mutable so
    // appBufAvailable() can stay const-honest: the cache is logically
    // an internal detail of the pop/peek path, and all callers must
    // already hold the protocol lock. Marking it mutable documents
    // that contract and lets the compiler enforce const correctness
    // at every other call site.
    mutable int peek_buf = -1;
    // Extended peek buffer for peekAt(). read N bytes into peek_buf_,
    // track valid range in peek_buf_len_, drain via pop/peek.
    static constexpr int PEEK_BUF_CAP = 16;
    mutable uint8_t peek_buf_[PEEK_BUF_CAP];
    mutable int     peek_buf_len_ = 0;
    mutable int     peek_buf_pos_ = 0;
    // One-shot guard for the pushAppBuf-when-NULL diagnostic log so
    // the operator sees one error line per session, not a flood.
    mutable bool appBufNullLogged_ = false;
    
    uart_config_t uart_config;

    static void uart_event_task(void *pvParameters) {
        EspHal* hal = (EspHal*)pvParameters;
        uart_event_t event;
        // Diagnostic: confirm the task is pinned to the intended core.
        // Before v3.0.9 this ran with no affinity at priority 12 and could
        // land on core 0, starving its idle task and tripping the WDT.
        Log::log().info(HAL_TAG,
            "uart_event_task running on core %d, priority %u",
            xPortGetCoreID(),
            (unsigned)uxTaskPriorityGet(NULL));
        // Heap scratch buffer sized to the per-event read cap. Allocated once
        // for the task's lifetime; alloca here would overflow the 4 KB stack
        // for large rxBufferSize configs.
        size_t rx_cap = hal->cfg.rxBufferSize;
        uint8_t* rx_buf = (uint8_t*)malloc(rx_cap);
        if (!rx_buf) {
            Log::log().error(HAL_TAG, "RX scratch alloc failed (%u B)", (unsigned)rx_cap);
            if (hal->task_exit_sem) xSemaphoreGive(hal->task_exit_sem);
            vTaskDelete(NULL);
            return;
        }

        // BREAK debounce: collapse breaks closer than 50 ms so a noise burst
        // can't bounce the link; legitimate peer breaks always pass.
        constexpr uint32_t BREAK_DEBOUNCE_MS = 50;
        uint32_t last_break_ms = 0;

        while(hal->running) {
            if(xQueueReceive(hal->uart_queue, (void * )&event, pdMS_TO_TICKS(100))) {
                if (!hal->running) break;
                if(event.type == UART_DATA) {
                    size_t read_len = (event.size > rx_cap) ? rx_cap : event.size;
                    int len = uart_read_bytes(hal->uart_num, rx_buf, read_len, portMAX_DELAY);
                    if(hal->link && len > 0) hal->link->onRx(rx_buf, len);
                }
                else if(event.type == UART_BREAK) {
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                    if ((uint32_t)(now - last_break_ms) < BREAK_DEBOUNCE_MS) {
                        // Spurious: drop the event but still drain the FIFO so
                        // any noise that arrived in the same burst doesn't
                        // get parsed as data after the new baud takes hold.
                        uart_flush_input(hal->uart_num);
                        continue;
                    }
                    last_break_ms = now;
                    uart_flush_input(hal->uart_num);
                    if(hal->link) hal->link->onBreak();
                }
            }
        }
        free(rx_buf);
        if (hal->task_exit_sem) xSemaphoreGive(hal->task_exit_sem);
        vTaskDelete(NULL);
    }

    static void timer_callback(TimerHandle_t xTimer) {
        EspHal* hal = (EspHal*) pvTimerGetTimerID(xTimer);
        // Direct call, safe because state modifications are locked
        if (hal && hal->link) hal->link->onTimer();
    }

public:
    EspHal(uart_port_t u_num, int rx_pin, int tx_pin, const AutoLinkConfig& config)
        : uart_num(u_num), rx_pin(rx_pin), tx_pin(tx_pin), cfg(config) {
        // v5.1.45 (post-audit fix): RTOS primitives (mutex, binary
        // semaphore, stream buffer) used to be allocated here. That
        // was safe IF EspHal was constructed lazily inside begin().
        // But user sketches put `PingPong upp(...)` at namespace
        // scope — which Arduino's startup hoists into .init_array,
        // running BEFORE the FreeRTOS scheduler is up. The ctor
        // then called xSemaphoreCreateMutex() / xStreamBufferCreate()
        // on a kernel that wasn't initialized, and the very first
        // timer interrupt fired into vApplicationGetTimerTaskMemory
        // with pxTCBBufferTemp == NULL — abort loop.
        //
        // Lesson (v5.1.17): the v5.1.17 Log& bug had the same shape
        // (namespace-scope reference bound to a singleton whose
        // ctor touched the heap before RTOS was up). The fix there
        // was a Meyers singleton — defer ctor to first call. Same
        // shape here, same fix shape: store config + zero-fill the
        // config struct in the ctor (cheap, no RTOS), and let
        // begin() do the RTOS allocations after setup() runs.
        //
        // Zero first so fields added/removed across ESP-IDF versions start known.
        memset(&uart_config, 0, sizeof(uart_config_t));

        uart_config.baud_rate = 9600;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;
    }
    
    void begin() override {
        if (running) return;
        running = true;

        // v5.1.45 (post-audit fix): RTOS primitives used to be
        // allocated in the ctor (see ctor comment for the boot-loop
        // post-mortem). Moved here so they fire AFTER setup() —
        // i.e. after FreeRTOS is fully up and the scheduler is
        // running. Idempotent: begin() is guarded by `running`.
        if (!mutex) mutex = xSemaphoreCreateMutex();
        if (!task_exit_sem) task_exit_sem = xSemaphoreCreateBinary();
        // xStreamBufferCreate returns NULL on heap fragmentation.
        // The symptom is "app buffer full" on the first data frame
        // after link-up — because pushAppBuf returns 0 when
        // stream_buf is NULL — which blames the wire instead of the
        // real cause.
        if (!stream_buf) {
            stream_buf = xStreamBufferCreate(cfg.streamBufferSize, 1);
            if (!stream_buf) {
                Log::log().error(HAL_TAG,
                    "xStreamBufferCreate failed: requested %u bytes, app buffer disabled. "
                    "Increase configSUPPORT_DYNAMIC_ALLOCATION or reduce streamBufferSize.",
                    (unsigned)cfg.streamBufferSize);
            }
        }

        auto cleanup_resources = [&]() {
            running = false;
            if(mutex) { vSemaphoreDelete(mutex); mutex = nullptr; }
            if(task_exit_sem) { vSemaphoreDelete(task_exit_sem); task_exit_sem = nullptr; }
            if(stream_buf) { vStreamBufferDelete(stream_buf); stream_buf = nullptr; }
        };

        if (uart_driver_install(uart_num, cfg.rxBufferSize, cfg.txBufferSize, 10, &uart_queue, 0) != ESP_OK) {
            Log::log().error(HAL_TAG, "Failed to install UART driver");
            cleanup_resources();
            return;
        }
        if (uart_param_config(uart_num, &uart_config) != ESP_OK) {
            Log::log().error(HAL_TAG,
                "uart_param_config failed for UART%d (baud %lu)",
                (int)uart_num, (unsigned long)uart_config.baud_rate);
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        if (uart_set_pin(uart_num, tx_pin, rx_pin,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            Log::log().error(HAL_TAG,
                "uart_set_pin failed for UART%d tx=%d rx=%d — "
                "check these GPIO numbers exist on your board "
                "(see your board's pinout for valid GPIO assignments).",
                (int)uart_num, tx_pin, rx_pin);
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        Log::log().info(HAL_TAG, "UART%d ready: tx=GPIO%d rx=GPIO%d",
            (int)uart_num, tx_pin, rx_pin);

        // Pull the RX pin high so an unconnected line is a stable UART idle
        // (mark = HIGH). Without this a floating pin generates a continuous
        // stream of noise bytes — ~11 kB/s at 115200, ~375 kB/s at 3 MHz —
        // which floods the event queue and can hang the system.
        gpio_set_pull_mode((gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
        
        // Pin to core 1 (same as Arduino loop()) so core 0's idle task
        // always runs freely. Priority 5 is well above loop() (1) but
        // leaves headroom for system tasks; priority 12 was starving the
        // idle task on core 0 and triggering the Task Watchdog (~20 s).
        if (xTaskCreatePinnedToCore(uart_event_task, "uart_ev_task", 4096,
                                    this, 5, &task_handle, 1) != pdPASS) {
            Log::log().error(HAL_TAG, "Failed to create UART event task");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this, timer_callback);
        if (timer_handle == NULL) {
            // xTimerCreate failed. UART event task + driver already
            // running — unwind the exact same teardown every other
            // failure path runs.
            Log::log().error(HAL_TAG, "Failed to create FreeRTOS timer");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        healthy = true;
        // Kick off baud negotiation now that the HAL is fully running.
        if (link) link->begin();
    }
    
    ~EspHal() {
        running = false;
        if (task_handle) {
            if (task_exit_sem) xSemaphoreTake(task_exit_sem, portMAX_DELAY);
        }
        if (task_exit_sem) vSemaphoreDelete(task_exit_sem);
        if(timer_handle) xTimerDelete(timer_handle, 0);
        uart_driver_delete(uart_num); 
        if(stream_buf) vStreamBufferDelete(stream_buf);
        if(mutex) vSemaphoreDelete(mutex);
    }
    
    bool isHealthy() const { return healthy; }

    void setSpd(uint32_t spd) override {
        // Flush stale old-baud samples so they don't parse as garbage and
        // trip the err threshold on a clean re-negotiation.
        uart_flush_input(uart_num);
        esp_err_t e = uart_set_baudrate(uart_num, spd);
        if (e != ESP_OK) {
            // Next sweep tick will retry at a different baud; this is
            // logged so the operator can tell setSpd failures apart
            // from genuine wiring errors.
            Log::log().error(HAL_TAG,
                "uart_set_baudrate(%lu) failed (err=0x%X). UART driver may "
                "not support this baud on the current clock config.",
                (unsigned long)spd, (unsigned)e);
        }
    }
    void sendBreak() override { uart_write_bytes_with_break(uart_num, " ", 1, 15); }
    int tx(const uint8_t* b, int n) override { return uart_write_bytes(uart_num, (const char*)b, n); }
    void flushTx() override { uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100)); }
    
    void startTimer(int ms) override {
        if(!timer_handle) return;
        // xTimerChangePeriod + xTimerStart post to the timer-service command
        // queue. Under a rapid drop->sweep->drop storm that queue can fill;
        // with a 0 block time either command is silently dropped and the
        // sweep timer never restarts, wedging the node in SWP with no PINGs
        // going out (the peer then reads 0 raw bytes and prints a false
        // WIRING CHECK). Block briefly so commands actually land, and treat
        // ChangePeriod + Start as a single transaction: if ChangePeriod
        // fails, do not Start (the timer would be left at the old period,
        // and a successful Start after a failed ChangePeriod is worse than
        // no Start at all — the caller would observe a different cadence
        // than what it asked for). Retry the whole pair a couple of times
        // so a momentarily-full command queue still resolves cleanly.
        const TickType_t block = pdMS_TO_TICKS(20);
        const TickType_t period = pdMS_TO_TICKS(ms);
        for (int i = 0; i < 3; i++) {
            if (xTimerChangePeriod(timer_handle, period, block) == pdPASS &&
                xTimerStart(timer_handle, block) == pdPASS) return;
        }
        Log::log().error(HAL_TAG,
            "startTimer(%d ms) gave up: timer-service command queue full",
            ms);
    }
    void stopTimer() override {
        if(timer_handle) xTimerStop(timer_handle, pdMS_TO_TICKS(20));
    }
    
    void delayMs(int ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    uint32_t nowMs() override {
#ifdef ARDUINO
        return (uint32_t)millis();
#else
        return (uint32_t)(esp_timer_get_time() / 1000);
#endif
    }
    
    void lock() const override { 
        if(mutex) xSemaphoreTake(mutex, portMAX_DELAY); 
    }
    void unlock() const override { 
        if(mutex) xSemaphoreGive(mutex); 
    }
    
    int pushAppBuf(const uint8_t* b, int n) override {
        if (n <= 0) return 0;
        // If xStreamBufferCreate failed at boot, every push silently
        // returns 0. The caller would log "app buffer full" which
        // blames the wire — log a one-shot error so the real cause
        // is visible.
        if (!stream_buf) {
            if (!appBufNullLogged_) {
                Log::log().error(HAL_TAG,
                    "pushAppBuf: stream_buf is NULL (xStreamBufferCreate failed at boot). "
                    "All RX payloads will be dropped. Check heap / streamBufferSize.");
                appBufNullLogged_ = true;
            }
            return 0;
        }
        // Returns bytes accepted; a shortfall means the app buffer is full
        // and the caller counts the loss as a link error.
        return (int)xStreamBufferSend(stream_buf, b, n, 0);
    }

    int popAppBuf(uint8_t* b, int max_len) override {
        int total = 0;
        // Drain from peek_buf_ (multi-byte) before falling through to
        // the single-byte peek_buf legacy cache and the stream buffer.
        while (total < max_len && peek_buf_pos_ < peek_buf_len_) {
            b[total++] = peek_buf_[peek_buf_pos_++];
        }
        if (peek_buf_pos_ >= peek_buf_len_) {
            peek_buf_len_ = 0;
            peek_buf_pos_ = 0;
        }
        // peek_buf (single-byte legacy) is independent of peek_buf_.
        if (total < max_len && peek_buf != -1) {
            b[total++] = (uint8_t)peek_buf;
            peek_buf = -1;
        }
        if (total < max_len && stream_buf) {
            size_t recv = xStreamBufferReceive(stream_buf, b + total, max_len - total, 0);
            total += recv;
        }
        return total;
    }

    int peekAppBuf() const override {
        if (peek_buf_pos_ < peek_buf_len_) return peek_buf_[peek_buf_pos_];
        if (peek_buf == -1) {
            uint8_t b;
            if (stream_buf && xStreamBufferReceive(stream_buf, &b, 1, 0) == 1) peek_buf = b;
        }
        return peek_buf;
    }

    // Peek N bytes from the app buffer at offset. Bytes 0..peek_buf_len_-1
    // are served from peek_buf_; the rest from the stream buffer via a
    // 1-byte temporary read+cache loop. Callers MUST hold the ALink lock.
    int peekAt(uint8_t* out, int n, int offset) const override {
        if (n <= 0 || offset < 0) return 0;
        int copied = 0;
        int pos = offset;
        // If the offset falls inside peek_buf_, start from there.
        if (pos < peek_buf_len_) {
            int fromBuf = std::min(n, peek_buf_len_ - pos);
            memcpy(out, peek_buf_ + pos, fromBuf);
            copied += fromBuf;
            pos = 0;
        } else {
            pos -= peek_buf_len_;
        }
        // Drain the rest from the stream buffer. Each byte: receive
        // 1 byte into `tmp`, then write it into out[copied]. The
        // write to peek_buf_ (below) preserves the byte order.
        while (copied < n) {
            uint8_t tmp;
            if (!stream_buf) break;
            if (xStreamBufferReceive(stream_buf, &tmp, 1, 0) != 1) break;
            out[copied++] = tmp;
            // Cache the byte so subsequent pop/peek starts with it.
            if (peek_buf_len_ < PEEK_BUF_CAP) {
                peek_buf_[peek_buf_len_++] = tmp;
            }
        }
        return copied;
    }
    int appBufAvailable() const override {
        int n = stream_buf ? xStreamBufferBytesAvailable(stream_buf) : 0;
        if (peek_buf != -1) n++;
        // Note: peek_buf_ contents are already counted by the
        // xStreamBufferBytesAvailable call (the receive above
        // removed them from the stream buffer). We don't add
        // peek_buf_len_ separately here to avoid double-counting.
        return n;
    }
    void clearAppBuf() override {
        if(stream_buf) xStreamBufferReset(stream_buf);
        peek_buf = -1;
        peek_buf_len_ = 0;
        peek_buf_pos_ = 0;
    }
    // Flush the UART driver ring buffer so bytes already received by the
    // hardware but not yet pumped into the stream buffer are discarded.
    // Called alongside clearAppBuf() from ALink::flushRx().
    void flushRxHw() override {
        uart_flush_input(uart_num);
    }
};

} // namespace autolink

