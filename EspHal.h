#pragma once
#include "ILink.h"
#include "ALink.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

namespace autolink {

class EspHal : public ILink {
    static constexpr size_t MAX_RX_BUFFER = 512;
    static constexpr const char* HAL_TAG = "EspHal";

    uart_port_t uart_num;
    QueueHandle_t uart_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    TimerHandle_t timer_handle = nullptr;
    StreamBufferHandle_t stream_buf = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t task_exit_sem = nullptr;
    volatile bool running = false;
    bool healthy = false;
    int peek_buf = -1;
    
    uint8_t rx_buf[MAX_RX_BUFFER];

    static void uart_event_task(void *pvParameters) {
        EspHal* hal = (EspHal*)pvParameters;
        uart_event_t event;
        
        while(hal->running) {
            if(xQueueReceive(hal->uart_queue, (void * )&event, pdMS_TO_TICKS(100))) {
                if (!hal->running) break;
                if(event.type == UART_DATA) {
                    size_t read_len = (event.size > MAX_RX_BUFFER) ? MAX_RX_BUFFER : event.size;
                    int len = uart_read_bytes(hal->uart_num, hal->rx_buf, read_len, portMAX_DELAY);
                    if(hal->link && len > 0) hal->link->onRx(hal->rx_buf, len);
                } 
                else if(event.type == UART_BREAK) {
                    uart_flush_input(hal->uart_num);
                    if(hal->link) hal->link->onBreak();
                }
                else if(event.type == (uart_event_type_t)UART_EVENT_MAX) {
                    if(hal->link) hal->link->onTimer();
                }
            }
        }
        if (hal->task_exit_sem) xSemaphoreGive(hal->task_exit_sem);
        vTaskDelete(NULL);
    }

    static void timer_callback(TimerHandle_t xTimer) {
        EspHal* hal = (EspHal*) pvTimerGetTimerID(xTimer);
        uart_event_t event;
        event.type = (uart_event_type_t)UART_EVENT_MAX; 
        if (hal->uart_queue) xQueueSend(hal->uart_queue, &event, 0);
    }

public:
    EspHal(uart_port_t u_num, int rx_pin, int tx_pin) : uart_num(u_num) {
        mutex = xSemaphoreCreateMutex();
        task_exit_sem = xSemaphoreCreateBinary();
        stream_buf = xStreamBufferCreate(1024, 1);
        running = true;
        
        uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 122,
            .use_ref_tick = false
        };
        
        auto cleanup_resources = [&]() {
            running = false;
            if(mutex) { vSemaphoreDelete(mutex); mutex = nullptr; }
            if(task_exit_sem) { vSemaphoreDelete(task_exit_sem); task_exit_sem = nullptr; }
            if(stream_buf) { vStreamBufferDelete(stream_buf); stream_buf = nullptr; }
        };

        if (uart_driver_install(uart_num, MAX_RX_BUFFER, MAX_RX_BUFFER, 10, &uart_queue, 0) != ESP_OK) {
            ESP_LOGE(HAL_TAG, "Failed to install UART driver");
            cleanup_resources();
            return;
        }
        uart_param_config(uart_num, &uart_config);
        uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        
        if (xTaskCreate(uart_event_task, "uart_ev_task", 4096, this, 12, &task_handle) != pdPASS) {
            ESP_LOGE(HAL_TAG, "Failed to create UART event task");
            uart_driver_delete(uart_num);
            cleanup_resources();
            return;
        }
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this, timer_callback);
        if (timer_handle == NULL) {
            ESP_LOGE(HAL_TAG, "Failed to create FreeRTOS timer");
            running = false;
            return;
        }
        healthy = true;
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

    void setSpd(uint32_t spd) override { uart_set_baudrate(uart_num, spd); }
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
    
    void lock() const override { 
        if(mutex) xSemaphoreTake(mutex, portMAX_DELAY); 
    }
    void unlock() const override { 
        if(mutex) xSemaphoreGive(mutex); 
    }
    
    void pushAppBuf(uint8_t b) override { 
        if(stream_buf) xStreamBufferSend(stream_buf, &b, 1, 0); 
    }
    int popAppBuf() override {
        if (peek_buf != -1) {
            int b = peek_buf;
            peek_buf = -1;
            return b;
        }
        uint8_t b;
        if (stream_buf && xStreamBufferReceive(stream_buf, &b, 1, 0) == 1) return b;
        return -1;
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
