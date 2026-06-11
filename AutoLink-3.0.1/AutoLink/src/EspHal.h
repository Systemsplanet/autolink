// EspHal.h — ESP32 implementation of the ILink hardware abstraction.
//
// Owns the UART driver, FreeRTOS event task, software timer, mutex, and
// stream buffer. Constructed by AutoLink; never used directly in sketches.
// For host testing use MockHal (defined in test.cpp) instead.
#pragma once
#include "ILink.h"
#include "ALink.h"
#include "Log.h"
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
#include <vector>
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
    int peek_buf = -1;
    
    uart_config_t uart_config;

    static void uart_event_task(void *pvParameters) {
        EspHal* hal = (EspHal*)pvParameters;
        uart_event_t event;
        // Heap scratch buffer sized to the per-event read cap. Allocated once
        // for the task's lifetime; alloca here would overflow the 4 KB stack
        // for large rxBufferSize configs.
        size_t rx_cap = hal->cfg.rxBufferSize;
        uint8_t* rx_buf = (uint8_t*)malloc(rx_cap);
        if (!rx_buf) {
            Log::getLog().error(HAL_TAG, "RX scratch alloc failed (%u B)", (unsigned)rx_cap);
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
        mutex = xSemaphoreCreateMutex();
        task_exit_sem = xSemaphoreCreateBinary();
        stream_buf = xStreamBufferCreate(cfg.streamBufferSize, 1);
        
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
        
        auto cleanup_resources = [&]() {
            running = false;
            if(mutex) { vSemaphoreDelete(mutex); mutex = nullptr; }
            if(task_exit_sem) { vSemaphoreDelete(task_exit_sem); task_exit_sem = nullptr; }
            if(stream_buf) { vStreamBufferDelete(stream_buf); stream_buf = nullptr; }
        };

        if (uart_driver_install(uart_num, cfg.rxBufferSize, cfg.rxBufferSize, 10, &uart_queue, 0) != ESP_OK) {
            Log::getLog().error(HAL_TAG, "Failed to install UART driver");
            cleanup_resources();
            return;
        }
        if (uart_param_config(uart_num, &uart_config) != ESP_OK) {
            Log::getLog().error(HAL_TAG,
                "uart_param_config failed for UART%d (baud %lu)",
                (int)uart_num, (unsigned long)uart_config.baud_rate);
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        if (uart_set_pin(uart_num, tx_pin, rx_pin,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            Log::getLog().error(HAL_TAG,
                "uart_set_pin failed for UART%d tx=%d rx=%d — "
                "check these GPIO numbers exist on your board. "
                "FireBeetle ESP32: GPIO17=D10(TX), GPIO16=D11(RX) on the header.",
                (int)uart_num, tx_pin, rx_pin);
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        Log::getLog().info(HAL_TAG, "UART%d ready: tx=GPIO%d rx=GPIO%d",
            (int)uart_num, tx_pin, rx_pin);

        // Pull the RX pin high so an unconnected line is a stable UART idle
        // (mark = HIGH). Without this a floating pin generates a continuous
        // stream of noise bytes — ~11 kB/s at 115200, ~375 kB/s at 3 MHz —
        // which floods the event queue and can hang the system.
        gpio_set_pull_mode((gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
        
        if (xTaskCreate(uart_event_task, "uart_ev_task", 4096, this, 12, &task_handle) != pdPASS) {
            Log::getLog().error(HAL_TAG, "Failed to create UART event task");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this, timer_callback);
        if (timer_handle == NULL) {
            Log::getLog().error(HAL_TAG, "Failed to create FreeRTOS timer");
            running = false;
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
        uart_set_baudrate(uart_num, spd);
    }
    void sendBreak() override { uart_write_bytes_with_break(uart_num, " ", 1, 15); }
    void tx(const uint8_t* b, int n) override { uart_write_bytes(uart_num, (const char*)b, n); }
    void flushTx() override { uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100)); }
    
    void startTimer(int ms) override {
        if(timer_handle) {
            xTimerChangePeriod(timer_handle, pdMS_TO_TICKS(ms), 0);
            xTimerStart(timer_handle, 0);
        }
    }
    void stopTimer() override { 
        if(timer_handle) xTimerStop(timer_handle, 0); 
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
    
    void pushAppBuf(uint8_t b) override { 
        if(stream_buf) xStreamBufferSend(stream_buf, &b, 1, 0); 
    }
    int pushAppBuf(const uint8_t* b, int n) override {
        if (!stream_buf || n <= 0) return 0;
        // Returns bytes accepted; a shortfall means the app buffer is full
        // and the caller counts the loss as a link error.
        return (int)xStreamBufferSend(stream_buf, b, n, 0);
    }
    
    int popAppBuf() override {
        uint8_t b;
        if (popAppBuf(&b, 1) == 1) return b;
        return -1;
    }
    int popAppBuf(uint8_t* b, int max_len) override {
        int total = 0;
        if (peek_buf != -1 && max_len > 0) {
            b[0] = peek_buf;
            peek_buf = -1;
            total = 1;
        }
        if (total < max_len && stream_buf) {
            size_t recv = xStreamBufferReceive(stream_buf, b + total, max_len - total, 0);
            total += recv;
        }
        return total;
    }
    
    int peekAppBuf() override {
        if (peek_buf == -1) {
            uint8_t b;
            if (stream_buf && xStreamBufferReceive(stream_buf, &b, 1, 0) == 1) peek_buf = b;
        }
        return peek_buf;
    }
    int appBufAvailable() const override { 
        int n = stream_buf ? xStreamBufferBytesAvailable(stream_buf) : 0; 
        if (peek_buf != -1) n++;
        return n;
    }
    void clearAppBuf() override { 
        if(stream_buf) xStreamBufferReset(stream_buf); 
        peek_buf = -1;
    }
};

} // namespace autolink

