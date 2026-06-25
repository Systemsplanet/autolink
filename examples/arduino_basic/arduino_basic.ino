// arduino_basic.ino — raw UART loopback diagnostic
// using the same ESP-IDF APIs as AutoLink (event
// queue, GPIO pull-up, uart_wait_tx_done, esp_timer
// LED).
//
// Uses NO AutoLink library code. Flash on BOTH boards
// — each one sends its own messages and echoes what it
// receives, so you see both wire directions from
// either serial monitor.
//
// LED feedback (blue LED, GPIO 2) — driven by
// esp_timer one-shots, same as AutoLink's EspBlinkHal:
//   Short flash  50 ms  — message sent
//   Long  flash 500 ms  — message received
//
// Wiring (crossover — same as AutoLink):
//   Board A  GPIO17 (TX) ──► Board B  GPIO16 (RX)
//   Board A  GPIO16 (RX) ◄── Board B  GPIO17 (TX)
//   shared GND
//
// Good output:                         LED:
//   > 00:00:01 Send: #1                short flash
//   < 00:00:01 Send: #1                long flash

#include <Arduino.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static constexpr uint32_t DEBUG_BAUD = 115200;
static constexpr uart_port_t UART_PORT = UART_NUM_2;
static constexpr uint32_t UART_BAUD = 115200;
static constexpr int RX_PIN = 16;
static constexpr int TX_PIN = 17;
static constexpr int LED_PIN = 2;
static constexpr int RX_BUF = 1024;
static constexpr int QUEUE_DEPTH = 10;

static QueueHandle_t uartQueue = nullptr;
static esp_timer_handle_t ledTimer = nullptr;
static uint32_t msgNum = 0;

// ── LED — non-blocking, esp_timer one-shot (same
// mechanism as AutoLink) ───────
//
// ledOn() lights the LED and schedules a one-shot
// timer to turn it off after `ms` milliseconds. A new
// call while the LED is on just restarts the timer
// (the longer flash wins if called twice in quick
// succession).

static void ledOffCb(void *)
{
    digitalWrite(LED_PIN, LOW);
}

static void ledOn(uint32_t ms)
{
    digitalWrite(LED_PIN, HIGH);
    esp_timer_stop(ledTimer);
    esp_timer_start_once(ledTimer, (uint64_t)ms * 1000ULL);
}

// ── timestamp
// ─────────────────────────────────────────────────────────────────

static void ts(char *buf, size_t len)
{
    uint32_t s = millis() / 1000;
    snprintf(buf, len, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
             (unsigned long)(s % 3600 / 60), (unsigned long)(s % 60));
}

// ── setup
// ─────────────────────────────────────────────────────────────────────

void setup()
{
    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    esp_timer_create_args_t ta = {};
    ta.callback = ledOffCb;
    ta.name = "diag_led";
    esp_timer_create(&ta, &ledTimer);

    Serial.begin(DEBUG_BAUD);
    delay(400);
    Serial.println("DiagEcho starting");

    // UART config — identical field layout to
    // EspHal::EspHal()
    uart_config_t cfg = {};
    cfg.baud_rate = UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 122;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // Install driver with event queue — same as
    // AutoLink's EspHal
    esp_err_t err = uart_driver_install(UART_PORT, RX_BUF, RX_BUF, QUEUE_DEPTH,
                                        &uartQueue, 0);
    if (err != ESP_OK) {
        Serial.printf("ERROR: uart_driver_install failed (%d)\n", err);
        while (true)
            delay(1000);
    }
    err = uart_param_config(UART_PORT, &cfg);
    if (err != ESP_OK) {
        Serial.printf("ERROR: uart_param_config failed (%d)\n", err);
        while (true)
            delay(1000);
    }
    err = uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        Serial.printf("ERROR: uart_set_pin failed (%d)\n", err);
        while (true)
            delay(1000);
    }

    // Pull RX high — same as AutoLink's EspHal,
    // prevents noise on floating pin
    gpio_set_pull_mode((gpio_num_t)RX_PIN, GPIO_PULLUP_ONLY);

    Serial.printf("UART%d ready  tx=GPIO%d  rx=GPIO%d  baud=%lu  queue=%d\n",
                  (int)UART_PORT, TX_PIN, RX_PIN, (unsigned long)UART_BAUD,
                  QUEUE_DEPTH);
}

// ── loop
// ──────────────────────────────────────────────────────────────────────

void loop()
{
    char tsBuf[12];
    ts(tsBuf, sizeof(tsBuf));

    // --- send ---
    char msg[64];
    snprintf(msg, sizeof(msg), "%s Send: #%lu\n", tsBuf,
             (unsigned long)++msgNum);
    uart_write_bytes(UART_PORT, msg, strlen(msg));
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
    Serial.printf("> %s", msg);
    ledOn(50); // short flash = sent

    // --- read via event queue (same path as
    // AutoLink's uart_event_task) ---
    uart_event_t event;
    bool gotSomething = false;
    uint32_t deadline = millis() + 950;

    while (millis() < deadline) {
        TickType_t wait = pdMS_TO_TICKS(deadline - millis() + 1);
        if (xQueueReceive(uartQueue, &event, wait) == pdTRUE) {
            if (event.type == UART_DATA && event.size > 0) {
                uint8_t rxBuf[RX_BUF];
                int got = uart_read_bytes(UART_PORT, rxBuf,
                                          event.size < (size_t)(RX_BUF - 1)
                                              ? event.size
                                              : (size_t)(RX_BUF - 1),
                                          pdMS_TO_TICKS(10));
                if (got > 0) {
                    rxBuf[got] = '\0';
                    Serial.printf("< %s", (char *)rxBuf);
                    ledOn(500); // long flash = received
                    gotSomething = true;
                }
            } else if (event.type == UART_BREAK) {
                Serial.println("< [BREAK signal received]");
                ledOn(500);
                gotSomething = true;
            }
        } else {
            break;
        }
    }

    if (!gotSomething) {
        Serial.println("< (nothing received)");
    }
}
