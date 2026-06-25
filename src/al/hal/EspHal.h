// IHal over ESP-IDF UART + FreeRTOS.
// UART events demux on core 1; Link runs on
// the calling task.
// RTOS primitives allocated in begin(), not
// the ctor — ctors run before app_main when
// the object is at namespace scope.
#pragma once
#include "al/AutoLinkConfig.h"
#include "al/hal/IHal.h"
#include "al/util/Log.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
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

    QueueHandle_t uart_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    TimerHandle_t timer_handle = nullptr;
    StreamBufferHandle_t stream_buf = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t task_exit_sem = nullptr;
    volatile bool running = false;
    bool healthy = false;

    // peek_buf_ holds bytes pulled ahead by
    // peekAt(); re-drained first by popAppBuf.
    mutable int peek_buf = -1;
    static constexpr int PEEK_BUF_CAP = 16;
    mutable uint8_t peek_buf_[PEEK_BUF_CAP];
    mutable int peek_buf_len_ = 0;
    mutable int peek_buf_pos_ = 0;
    mutable bool appBufNullLogged_ = false;

    uart_config_t uart_config;

    static void uart_event_task(void *pvParameters) {
        EspHal *hal = (EspHal *)pvParameters;
        uart_event_t event;
        Log::log().info(TAG, "uart_event_task core=%d", xPortGetCoreID());
        size_t rx_cap = hal->cfg.rxBufferSize;
        uint8_t *rx_buf = (uint8_t *)malloc(rx_cap);
        if (!rx_buf) {
            Log::log().error(TAG, "RX alloc failed (%uB)", (unsigned)rx_cap);
            if (hal->task_exit_sem)
                xSemaphoreGive(hal->task_exit_sem);
            vTaskDelete(NULL);
            return;
        }

        // Ignore a second BREAK within
        // this window of the first.
        constexpr uint32_t BREAK_DEBOUNCE_MS = 50;
        uint32_t last_break_ms = 0;

        while (hal->running) {
            if (xQueueReceive(hal->uart_queue, (void *)&event,
                              pdMS_TO_TICKS(100))) {
                if (!hal->running)
                    break;
                if (event.type == UART_DATA) {
                    size_t rlen = (event.size > rx_cap) ? rx_cap : event.size;
                    int len = uart_read_bytes(hal->uart_num, rx_buf, rlen,
                                              portMAX_DELAY);
                    if (hal->link && len > 0)
                        hal->link->onRx(rx_buf, len);
                } else if (event.type == UART_BREAK) {
                    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                    if ((uint32_t)(now - last_break_ms) < BREAK_DEBOUNCE_MS) {
                        uart_flush_input(hal->uart_num);
                        continue;
                    }
                    last_break_ms = now;
                    uart_flush_input(hal->uart_num);
                    if (hal->link)
                        hal->link->onBreak();
                }
            }
        }
        free(rx_buf);
        if (hal->task_exit_sem)
            xSemaphoreGive(hal->task_exit_sem);
        vTaskDelete(NULL);
    }

    static void timer_callback(TimerHandle_t xTimer) {
        EspHal *hal = (EspHal *)pvTimerGetTimerID(xTimer);
        if (hal && hal->link)
            hal->link->onTimer();
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

    void begin() override {
        if (running)
            return;
        running = true;
        if (!mutex)
            mutex = xSemaphoreCreateMutex();
        if (!task_exit_sem)
            task_exit_sem = xSemaphoreCreateBinary();
        if (!stream_buf) {
            stream_buf = xStreamBufferCreate(cfg.streamBufferSize, 1);
            if (!stream_buf)
                Log::log().error(TAG,
                                 "xStreamBufferCreate"
                                 " failed (%uB) — "
                                 "check heap",
                                 (unsigned)cfg.streamBufferSize);
        }
        auto cleanup = [&]() {
            running = false;
            if (mutex) {
                vSemaphoreDelete(mutex);
                mutex = nullptr;
            }
            if (task_exit_sem) {
                vSemaphoreDelete(task_exit_sem);
                task_exit_sem = nullptr;
            }
            if (stream_buf) {
                vStreamBufferDelete(stream_buf);
                stream_buf = nullptr;
            }
        };
        // Dirty reboot / crash can leave a driver installed;
        // the is_installed guard covers the never-installed case.
        if (uart_is_driver_installed(uart_num)) {
            uart_driver_delete(uart_num);
        }
        esp_err_t e = uart_driver_install(uart_num, cfg.rxBufferSize,
                                          cfg.txBufferSize, 10, &uart_queue, 0);
        if (e == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            e = uart_driver_install(uart_num, cfg.rxBufferSize,
                                    cfg.txBufferSize, 10, &uart_queue, 0);
        }
        if (e != ESP_OK) {
            Log::log().error(TAG, "uart_driver_install: %s",
                             esp_err_to_name(e));
            cleanup();
            return;
        }
        e = uart_param_config(uart_num, &uart_config);
        if (e != ESP_OK) {
            Log::log().error(TAG, "uart_param_config: %s", esp_err_to_name(e));
            uart_driver_delete(uart_num);
            cleanup();
            return;
        }
        e = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE);
        if (e != ESP_OK) {
            Log::log().error(TAG,
                             "uart_set_pin tx=%d rx=%d"
                             ": %s",
                             tx_pin, rx_pin, esp_err_to_name(e));
            uart_driver_delete(uart_num);
            cleanup();
            return;
        }
        Log::log().info(TAG, "UART%d tx=GPIO%d rx=GPIO%d", (int)uart_num,
                        tx_pin, rx_pin);
        esp_err_t gp = gpio_set_pull_mode((gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
        if (gp != ESP_OK)
            Log::log().error(TAG, "pull_mode GPIO%d: %s", rx_pin,
                             esp_err_to_name(gp));
        if (xTaskCreatePinnedToCore(uart_event_task, "uart_ev_task", 4096, this,
                                    5, &task_handle, 1) != pdPASS) {
            Log::log().error(TAG, "UART task create failed");
            uart_driver_delete(uart_num);
            cleanup();
            return;
        }
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE,
                                    this, timer_callback);
        if (!timer_handle) {
            Log::log().error(TAG, "timer create failed");
            uart_driver_delete(uart_num);
            cleanup();
            return;
        }
        healthy = true;
        if (link)
            link->begin();
    }

    ~EspHal() {
        running = false;
        if (task_handle && task_exit_sem)
            xSemaphoreTake(task_exit_sem, portMAX_DELAY);
        if (task_exit_sem)
            vSemaphoreDelete(task_exit_sem);
        if (timer_handle)
            xTimerDelete(timer_handle, 0);
        uart_driver_delete(uart_num);
        if (stream_buf)
            vStreamBufferDelete(stream_buf);
        if (mutex)
            vSemaphoreDelete(mutex);
    }

    bool isHealthy() const override { return healthy; }

    void setSpd(uint32_t spd) override {
        // Flush before retune: stale bytes
        // at old baud corrupt new-baud frames.
        uart_flush_input(uart_num);
        esp_err_t e = uart_set_baudrate(uart_num, spd);
        if (e != ESP_OK)
            Log::log().error(TAG, "set_baudrate(%lu): %s", (unsigned long)spd,
                             esp_err_to_name(e));
    }

    void sendBreak() override {
        uart_write_bytes_with_break(uart_num, " ", 1, 15);
    }

    int tx(const uint8_t *b, int n) override {
        return uart_write_bytes(uart_num, (const char *)b, n);
    }

    void flushTx() override { uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100)); }

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
        if (total < max_len && peek_buf != -1) {
            b[total++] = (uint8_t)peek_buf;
            peek_buf = -1;
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
        if (peek_buf == -1 && stream_buf) {
            uint8_t b;
            if (xStreamBufferReceive(stream_buf, &b, 1, 0) == 1)
                peek_buf = b;
        }
        return peek_buf;
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
        if (peek_buf != -1)
            n++;
        return n;
    }

    void clearAppBuf() override {
        if (stream_buf)
            xStreamBufferReset(stream_buf);
        peek_buf = -1;
        peek_buf_len_ = 0;
        peek_buf_pos_ = 0;
    }

    void flushRxHw() override { uart_flush_input(uart_num); }
};

} // namespace autolink
