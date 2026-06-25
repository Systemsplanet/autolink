// ESP-IDF IHal implementation: UART driver + FreeRTOS
// event task + stream buffer app buf + FreeRTOS timer.
// The Link runs on the calling task; UART events are
// demuxed on core 1.
#pragma once
#include "al/hal/IHal.h"
#include "al/link/Link.h"
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
#include <string.h>
#include <stdlib.h>

namespace autolink
{
class EspHal : public IHal
{
    static constexpr const char *HAL_TAG = "EspHal";

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


    mutable int peek_buf = -1;
    static constexpr int PEEK_BUF_CAP = 16;
    mutable uint8_t peek_buf_[PEEK_BUF_CAP];
    mutable int peek_buf_len_ = 0;
    mutable int peek_buf_pos_ = 0;
    mutable bool appBufNullLogged_ = false;

    uart_config_t uart_config;

    static void uart_event_task(void *pvParameters)
    {
        EspHal *hal = (EspHal *)pvParameters;
        uart_event_t event;
        Log::log().info(
            HAL_TAG,
            "uart_event_task running on core %d, "
            "priority %u",
            xPortGetCoreID(),
            (unsigned)uxTaskPriorityGet(NULL));
        size_t rx_cap = hal->cfg.rxBufferSize;
        uint8_t *rx_buf = (uint8_t *)malloc(rx_cap);
        if (!rx_buf) {
            Log::log().error(
                HAL_TAG,
                "RX scratch alloc failed (%u B)",
                (unsigned)rx_cap);
            if (hal->task_exit_sem)
                xSemaphoreGive(hal->task_exit_sem);
            vTaskDelete(NULL);
            return;
        }


        constexpr uint32_t BREAK_DEBOUNCE_MS = 50;
        uint32_t last_break_ms = 0;

        while (hal->running) {
            if (xQueueReceive(hal->uart_queue,
                              (void *)&event,
                              pdMS_TO_TICKS(100))) {
                if (!hal->running)
                    break;
                if (event.type == UART_DATA) {
                    size_t read_len =
                        (event.size > rx_cap)
                        ? rx_cap
                        : event.size;
                    int len = uart_read_bytes(
                        hal->uart_num, rx_buf,
                        read_len, portMAX_DELAY);
                    if (hal->link && len > 0)
                        hal->link->onRx(rx_buf, len);
                } else if (event.type == UART_BREAK) {
                    uint32_t now =
                        (uint32_t)(esp_timer_get_time() /
                                   1000);
                    if ((uint32_t)(now -
                                   last_break_ms) <
                        BREAK_DEBOUNCE_MS) {
                        uart_flush_input(
                            hal->uart_num);
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

    static void timer_callback(TimerHandle_t xTimer)
    {
        EspHal *hal =
            (EspHal *)pvTimerGetTimerID(xTimer);
        if (hal && hal->link)
            hal->link->onTimer();
    }

public:
    // RTOS primitives are allocated in begin(), not
    // here — ctors run before app_main when a user
    // instantiates at namespace scope (Arduino hoists
    // globals into .init_array).
    EspHal(uart_port_t u_num, int rx_pin, int tx_pin,
           const AutoLinkConfig &config)
        : uart_num(u_num), rx_pin(rx_pin),
          tx_pin(tx_pin), cfg(config)
    {
        memset(&uart_config, 0, sizeof(uart_config_t));

        uart_config.baud_rate = 9600;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl =
            UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;
    }

    void begin() override
    {
        if (running)
            return;
        running = true;

        if (!mutex)
            mutex = xSemaphoreCreateMutex();
        if (!task_exit_sem)
            task_exit_sem = xSemaphoreCreateBinary();


        if (!stream_buf) {
            stream_buf = xStreamBufferCreate(
                cfg.streamBufferSize, 1);
            if (!stream_buf) {
                Log::log().error(
                    HAL_TAG,
                    "xStreamBufferCreate failed: "
                    "requested %u bytes, app buffer "
                    "disabled. "
                    "Increase "
                    "configSUPPORT_DYNAMIC_ALLOCATION "
                    "or reduce streamBufferSize.",
                    (unsigned)cfg.streamBufferSize);
            }
        }

        auto cleanup_resources = [&]() {
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

        esp_err_t e1 = uart_driver_install(
            uart_num, cfg.rxBufferSize,
            cfg.txBufferSize, 10, &uart_queue, 0);
        if (e1 != ESP_OK) {
            Log::log().error(
                HAL_TAG,
                "uart_driver_install failed: %s",
                esp_err_to_name(e1));
            cleanup_resources();
            return;
        }
        esp_err_t e2 =
            uart_param_config(uart_num, &uart_config);
        if (e2 != ESP_OK) {
            Log::log().error(
                HAL_TAG,
                "uart_param_config failed for UART%d "
                "(baud %lu): %s",
                (int)uart_num,
                (unsigned long)uart_config.baud_rate,
                esp_err_to_name(e2));
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        esp_err_t e3 = uart_set_pin(
            uart_num, tx_pin, rx_pin,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (e3 != ESP_OK) {
            Log::log().error(
                HAL_TAG,
                "uart_set_pin failed for UART%d tx=%d "
                "rx=%d: %s — "
                "check these GPIO numbers exist on "
                "your board "
                "(see your board's pinout for valid "
                "GPIO assignments).",
                (int)uart_num, tx_pin, rx_pin,
                esp_err_to_name(e3));
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        Log::log().info(
            HAL_TAG,
            "UART%d ready: tx=GPIO%d rx=GPIO%d",
            (int)uart_num, tx_pin, rx_pin);


        esp_err_t gp = gpio_set_pull_mode(
            (gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
        if (gp != ESP_OK) {
            Log::log().error(
                HAL_TAG,
                "gpio_set_pull_mode(GPIO%d) '
                'failed: %s',
                rx_pin, esp_err_to_name(gp));
        }


        if (xTaskCreatePinnedToCore(
                uart_event_task, "uart_ev_task", 4096,
                this, 5, &task_handle, 1) != pdPASS) {
            Log::log().error(
                HAL_TAG,
                "Failed to create UART event task");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        timer_handle = xTimerCreate(
            "alink_tmr", pdMS_TO_TICKS(50), pdFALSE,
            this, timer_callback);
        if (timer_handle == NULL) {
            Log::log().error(
                HAL_TAG,
                "Failed to create FreeRTOS timer");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        healthy = true;
        if (link)
            link->begin();
    }

    ~EspHal()
    {
        running = false;
        if (task_handle) {
            if (task_exit_sem)
                xSemaphoreTake(task_exit_sem,
                               portMAX_DELAY);
        }
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

    bool isHealthy() const { return healthy; }

    void setSpd(uint32_t spd) override
    {
        // Flush before retune: stale bytes from the
        // old baud confuse the frame decoder at the
        // new baud.


        uart_flush_input(uart_num);
        esp_err_t e = uart_set_baudrate(uart_num, spd);
        if (e != ESP_OK) {
            Log::log().error(
                HAL_TAG,
                "uart_set_baudrate(%lu) failed: %s. "
                "UART driver may "
                "not support this baud on the current "
                "clock config.",
                (unsigned long)spd,
                esp_err_to_name(e));
        }
    }
    void sendBreak() override
    {
        uart_write_bytes_with_break(uart_num, " ", 1,
                                    15);
    }
    int tx(const uint8_t *b, int n) override
    {
        return uart_write_bytes(uart_num,
                                (const char *)b, n);
    }
    void flushTx() override
    {
        uart_wait_tx_done(uart_num,
                          pdMS_TO_TICKS(100));
    }

    void startTimer(int ms) override
    {
        if (!timer_handle)
            return;


        const TickType_t block = pdMS_TO_TICKS(20);
        const TickType_t period = pdMS_TO_TICKS(ms);
        for (int i = 0; i < 3; i++) {
            if (xTimerChangePeriod(timer_handle,
                                   period,
                                   block) == pdPASS &&
                xTimerStart(timer_handle, block) ==
                    pdPASS)
                return;
        }
        Log::log().error(
            HAL_TAG,
            "startTimer(%d ms) gave up: timer-service "
            "command queue full",
            ms);
    }
    void stopTimer() override
    {
        if (timer_handle)
            xTimerStop(timer_handle,
                       pdMS_TO_TICKS(20));
    }

    void delayMs(int ms) override
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    uint32_t nowMs() override
    {
#ifdef ARDUINO
        return (uint32_t)millis();
#else
        return (uint32_t)(esp_timer_get_time() / 1000);
#endif
    }

    void lock() const override
    {
        if (mutex)
            xSemaphoreTake(mutex, portMAX_DELAY);
    }
    void unlock() const override
    {
        if (mutex)
            xSemaphoreGive(mutex);
    }

    int pushAppBuf(const uint8_t *b, int n) override
    {
        if (n <= 0)
            return 0;


        if (!stream_buf) {
            if (!appBufNullLogged_) {
                Log::log().error(
                    HAL_TAG,
                    "pushAppBuf: stream_buf is NULL "
                    "(xStreamBufferCreate failed at "
                    "boot). "
                    "All RX payloads will be dropped. "
                    "Check heap / streamBufferSize.");
                appBufNullLogged_ = true;
            }
            return 0;
        }
        return (int)xStreamBufferSend(stream_buf, b, n,
                                      0);
    }

    int popAppBuf(uint8_t *b, int max_len) override
    {
        int total = 0;
        while (total < max_len &&
               peek_buf_pos_ < peek_buf_len_) {
            b[total++] = peek_buf_[peek_buf_pos_++];
        }
        if (peek_buf_pos_ >= peek_buf_len_) {
            peek_buf_len_ = 0;
            peek_buf_pos_ = 0;
        }
        if (total < max_len && peek_buf != -1) {
            b[total++] = (uint8_t)peek_buf;
            peek_buf = -1;
        }
        if (total < max_len && stream_buf) {
            size_t recv = xStreamBufferReceive(
                stream_buf, b + total, max_len - total,
                0);
            total += recv;
        }
        return total;
    }

    int peekAppBuf() const override
    {
        if (peek_buf_pos_ < peek_buf_len_)
            return peek_buf_[peek_buf_pos_];
        if (peek_buf == -1) {
            uint8_t b;
            if (stream_buf &&
                xStreamBufferReceive(stream_buf, &b, 1,
                                     0) == 1)
                peek_buf = b;
        }
        return peek_buf;
    }


    int peekAt(uint8_t *out, int n,
               int offset) const override
    {
        if (n <= 0 || offset < 0)
            return 0;
        int copied = 0;
        int pos = offset;
        if (pos < peek_buf_len_) {
            int fromBuf =
                std::min(n, peek_buf_len_ - pos);
            memcpy(out, peek_buf_ + pos, fromBuf);
            copied += fromBuf;
            pos = 0;
        } else {
            pos -= peek_buf_len_;
        }
        while (copied < n) {
            uint8_t tmp;
            if (!stream_buf)
                break;
            if (xStreamBufferReceive(stream_buf, &tmp,
                                     1, 0) != 1)
                break;
            out[copied++] = tmp;
            if (peek_buf_len_ < PEEK_BUF_CAP) {
                peek_buf_[peek_buf_len_++] = tmp;
            }
        }
        return copied;
    }
    int appBufAvailable() const override
    {
        int n = stream_buf
            ? xStreamBufferBytesAvailable(stream_buf)
            : 0;
        if (peek_buf != -1)
            n++;
        return n;
    }
    void clearAppBuf() override
    {
        if (stream_buf)
            xStreamBufferReset(stream_buf);
        peek_buf = -1;
        peek_buf_len_ = 0;
        peek_buf_pos_ = 0;
    }


    void flushRxHw() override
    {
        uart_flush_input(uart_num);
    }
};

} // namespace autolink