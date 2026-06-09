#pragma once
#include "ILink.h"
#include "ALink.h"
#include "Log.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <vector>
#include <string.h> // memset, alloca

namespace autolink {

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
        // Static scratch buffer sized to the per-event read cap. Avoids the
        // std::vector alloc/ctor churn of the previous version on a 4 KB stack
        // task that runs forever.
        size_t rx_cap = hal->cfg.rxBufferSize;
        uint8_t* rx_buf = (uint8_t*)alloca(rx_cap);

        // BREAK debounce: a line glitch can fire UART_BREAK for a few hundred
        // microseconds. Two breaks closer than this many ms apart are
        // collapsed into one onBreak() so a single noise burst can't bounce
        // the link. 50 ms is well below the master's sweep tick (>= delayMs)
        // and below any human-initiated power-cycle, so legitimate breaks
        // always make it through.
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
        
        // Zero the struct first so any field added in newer ESP-IDF versions
        // (e.g. use_ref_tick was removed in v5) starts from a known value.
        memset(&uart_config, 0, sizeof(uart_config_t));

        uart_config.baud_rate = 9600;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;
        
        // Note: 'use_ref_tick' is intentionally omitted here as it was removed in ESP-IDF v5+.
        // Using memset above guarantees the struct is properly initialized without it.
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
        uart_param_config(uart_num, &uart_config);
        uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        
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
        // Flush stale RX samples before retuning. The protocol expects the
        // RX FIFO to contain only bytes received at the new baud; otherwise
        // the first bytes after a setSpd are at the old baud and parse as
        // garbage, which fires err_unlocked() on the very first byte of the
        // new sweep and trips the err threshold on a clean re-negotiation.
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
    
    void lock() const override { 
        if(mutex) xSemaphoreTake(mutex, portMAX_DELAY); 
    }
    void unlock() const override { 
        if(mutex) xSemaphoreGive(mutex); 
    }
    
    void pushAppBuf(uint8_t b) override { 
        if(stream_buf) xStreamBufferSend(stream_buf, &b, 1, 0); 
    }
    void pushAppBuf(const uint8_t* b, int n) override {
        if(stream_buf && n > 0) xStreamBufferSend(stream_buf, b, n, 0);
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

