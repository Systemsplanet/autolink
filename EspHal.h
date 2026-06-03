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

#define UART_MAX_RX_BUFFER 512
static const char* HAL_TAG = "EspHal";

class EspHal : public ILink {
    uart_port_t uart_num;
    QueueHandle_t uart_queue = nullptr;
    TaskHandle_t task_handle = nullptr;
    TimerHandle_t timer_handle = nullptr;
    StreamBufferHandle_t stream_buf = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    bool healthy = false;

    static void uart_event_task(void *pvParameters) {
        EspHal* hal = (EspHal*)pvParameters;
        uart_event_t event;
        uint8_t* dtmp = (uint8_t*) malloc(UART_MAX_RX_BUFFER);
        if(dtmp == NULL) {
            ESP_LOGE(HAL_TAG, "Failed to allocate memory for UART RX buffer. Task aborting.");
            vTaskDelete(NULL); 
        }
        
        while(1) {
            if(xQueueReceive(hal->uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
                if(event.type == UART_DATA) {
                    size_t read_len = (event.size > UART_MAX_RX_BUFFER) ? UART_MAX_RX_BUFFER : event.size;
                    int len = uart_read_bytes(hal->uart_num, dtmp, read_len, portMAX_DELAY);
                    if(hal->link && len > 0) hal->link->onRx(dtmp, len);
                } 
                else if(event.type == UART_BREAK) {
                    uart_flush_input(hal->uart_num);
                    if(hal->link) hal->link->onBreak();
                }
            }
        }
        free(dtmp);
    }

    static void timer_callback(TimerHandle_t xTimer) {
        EspHal* hal = (EspHal*) pvTimerGetTimerID(xTimer);
        if(hal->link) hal->link->onTimer();
    }

public:
    EspHal(uart_port_t u_num, int rx_pin, int tx_pin) : uart_num(u_num) {
        mutex = xSemaphoreCreateMutex();
        stream_buf = xStreamBufferCreate(1024, 1);
        
        uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 122,
            .use_ref_tick = false
        };
        
        if (uart_driver_install(uart_num, UART_MAX_RX_BUFFER, UART_MAX_RX_BUFFER, 10, &uart_queue, 0) != ESP_OK) {
            ESP_LOGE(HAL_TAG, "Failed to install UART driver");
            return;
        }
        uart_param_config(uart_num, &uart_config);
        uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        
        if (xTaskCreate(uart_event_task, "uart_ev_task", 4096, this, 12, &task_handle) != pdPASS) {
            ESP_LOGE(HAL_TAG, "Failed to create UART event task due to insufficient memory");
            return;
        }
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this, timer_callback);
        if (timer_handle == NULL) {
            ESP_LOGE(HAL_TAG, "Failed to create FreeRTOS timer");
            return;
        }
        healthy = true;
    }
    
    ~EspHal() {
        if(task_handle) vTaskDelete(task_handle);
        if(timer_handle) xTimerDelete(timer_handle, 0);
        uart_driver_delete(uart_num); 
        if(stream_buf) vStreamBufferDelete(stream_buf);
        if(mutex) vSemaphoreDelete(mutex);
    }
    
    bool isHealthy() const { return healthy; }

    void setSpd(int spd) override { uart_set_baudrate(uart_num, spd); }
    void sendBreak() override { uart_write_bytes_with_break(uart_num, "\0", 1, 15); }
    void tx(const uint8_t* b, int n) override { uart_write_bytes(uart_num, (const char*)b, n); }
    void flushTx() override { uart_wait_tx_done(uart_num, portMAX_DELAY); }
    
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
        uint8_t b;
        if (stream_buf && xStreamBufferReceive(stream_buf, &b, 1, 0) == 1) return b;
        return -1;
    }
    int appBufAvailable() const override { 
        return stream_buf ? xStreamBufferBytesAvailable(stream_buf) : 0; 
    }
    void clearAppBuf() override { 
        if(stream_buf) xStreamBufferReset(stream_buf); 
    }
};
