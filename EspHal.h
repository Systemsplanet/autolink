#pragma once
#include "ILink.h"
#include "ALink.h"
#include <Arduino.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

class EspHal : public ILink {
    uart_port_t uart_num;
    QueueHandle_t uart_queue;
    TaskHandle_t task_handle;
    TimerHandle_t timer_handle;

    static void uart_event_task(void *pvParameters) {
        EspHal* hal = (EspHal*)pvParameters;
        uart_event_t event;
        uint8_t* dtmp = (uint8_t*) malloc(256);
        
        while(1) {
            if(xQueueReceive(hal->uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
                if(event.type == UART_DATA) {
                    int len = uart_read_bytes(hal->uart_num, dtmp, event.size, portMAX_DELAY);
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
        uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
        };
        
        uart_driver_install(uart_num, 512, 512, 10, &uart_queue, 0);
        uart_param_config(uart_num, &uart_config);
        uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        
        xTaskCreate(uart_event_task, "uart_ev_task", 2048, this, 12, &task_handle);
        timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this, timer_callback);
    }
    
    ~EspHal() {
        vTaskDelete(task_handle);
        xTimerDelete(timer_handle, 0);
        uart_driver_delete(uart_num);
    }

    void setSpd(int spd) override { uart_set_baudrate(uart_num, spd); }
    void sendBreak() override { uart_write_bytes_with_break(uart_num, "\0", 1, 15); }
    void tx(const uint8_t* b, int n) override { uart_write_bytes(uart_num, (const char*)b, n); }
    void flushTx() override { uart_wait_tx_done(uart_num, portMAX_DELAY); }
    void startTimer(int ms) override {
        xTimerChangePeriod(timer_handle, pdMS_TO_TICKS(ms), 0);
        xTimerStart(timer_handle, 0);
    }
    void stopTimer() override { xTimerStop(timer_handle, 0); }
};
