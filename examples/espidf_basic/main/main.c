// Stock-ESP-IDF example: UART-driven echo on UART2
// with LED heartbeat. C counterpart to the Arduino
// basic sketch.
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define DEBUG_BAUD 115200
#define UART_PORT UART_NUM_2
#define UART_BAUD 115200
#define RX_PIN 16
#define TX_PIN 17
#define LED_PIN 2
#define RX_BUF 1024
#define QUEUE_DEPTH 10
#define TAG "espidf_basic"

static QueueHandle_t uart_queue = NULL;
static esp_timer_handle_t led_timer = NULL;
static uint32_t msg_num = 0;

static void led_off_cb(void *arg)
{
    (void)arg;
    gpio_set_level(LED_PIN, 0);
}

static void led_on(uint32_t ms)
{
    gpio_set_level(LED_PIN, 1);
    esp_timer_stop(led_timer);
    esp_timer_start_once(led_timer, (uint64_t)ms * 1000ULL);
}

static void timestamp(char *buf, size_t len)
{
    uint32_t s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    snprintf(buf, len, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, s / 3600,
             (s % 3600) / 60, s % 60);
}

void app_main(void)
{
    ESP_LOGI(TAG, "espidf_basic starting");

    gpio_config_t led_io = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);
    gpio_set_level(LED_PIN, 0);

    esp_timer_create_args_t ta = {
        .callback = led_off_cb,
        .name = "diag_led",
    };
    esp_timer_create(&ta, &led_timer);

    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_driver_install(UART_PORT, RX_BUF, RX_BUF, QUEUE_DEPTH, &uart_queue,
                            0) != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed");
        while (1)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (uart_param_config(UART_PORT, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed");
        while (1)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed");
        while (1)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    gpio_set_pull_mode(RX_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "UART%d ready  tx=GPIO%d  rx=GPIO%d  baud=%d  queue=%d",
             (int)UART_PORT, TX_PIN, RX_PIN, (int)UART_BAUD, QUEUE_DEPTH);

    while (1) {
        char ts_buf[12];
        timestamp(ts_buf, sizeof(ts_buf));

        char msg[64];
        int n = snprintf(msg, sizeof(msg), "%s Send: #%" PRIu32 "\n", ts_buf,
                         ++msg_num);
        uart_write_bytes(UART_PORT, msg, n);
        uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
        printf("> %s", msg);
        led_on(50);

        uart_event_t event;
        bool got_something = false;
        uint32_t deadline = xTaskGetTickCount() * portTICK_PERIOD_MS + 950;

        while ((xTaskGetTickCount() * portTICK_PERIOD_MS) < deadline) {
            uint32_t remaining =
                deadline - xTaskGetTickCount() * portTICK_PERIOD_MS + 1;
            TickType_t wait = pdMS_TO_TICKS(remaining);
            if (xQueueReceive(uart_queue, &event, wait) == pdTRUE) {
                if (event.type == UART_DATA && event.size > 0) {
                    uint8_t rx_buf[RX_BUF];
                    int got = uart_read_bytes(
                        UART_PORT, rx_buf,
                        event.size < (RX_BUF - 1) ? event.size : (RX_BUF - 1),
                        pdMS_TO_TICKS(10));
                    if (got > 0) {
                        rx_buf[got] = '\0';
                        printf("< %s", (char *)rx_buf);
                        led_on(500);
                        got_something = true;
                    }
                } else if (event.type == UART_BREAK) {
                    printf("< [BREAK signal received]\n");
                    led_on(500);
                    got_something = true;
                }
            } else {
                break;
            }
        }

        if (!got_something) {
            printf("< (nothing received)\n");
        }
    }
}
