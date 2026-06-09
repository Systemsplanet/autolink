#!/usr/bin/env python3
"""Extract the ```cpp blocks from README.md and syntax-check them with g++.
Catches example-code mistakes the host test build can't (test.cpp covers the
library, not the sketches). The Arduino/ESP-IDF surface needed to compile the
examples is embedded below and written to a temp dir at run time, so the repo
stays flat -- no host_stubs/ directory."""

import re
import sys
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent
readme = (ROOT / "README.md").read_text()

# ---------------------------------------------------------------------------
# Minimal Arduino / ESP-IDF stub headers, just enough surface for
# -fsyntax-only on AutoLink.h + the README sketches.
# ---------------------------------------------------------------------------
STUBS = {
    "Arduino.h": """\
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int  digitalRead(int pin);
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);
uint32_t esp_random();
void esp_log_level_set(const char* tag, int level);
#define ESP_LOG_VERBOSE 5
#define ESP_LOG_DEBUG 4
#define ESP_LOG_INFO 3
class HardwareSerial {
public:
    void begin(unsigned long baud);
    void println(const char* s);
    void print(const char* s);
    int  printf(const char* fmt, ...);
};
extern HardwareSerial Serial;
""",
    "Stream.h": """\
#pragma once
#include <stdint.h>
#include <stddef.h>
class Stream {
public:
    virtual ~Stream() {}
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* buffer, size_t size) = 0;
    virtual void flush() = 0;
};
""",
    "esp_err.h": """\
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
""",
    "esp_timer.h": """\
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_timer* esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void* arg);
typedef struct {
    esp_timer_cb_t callback;
    void* arg;
    int dispatch_method;
    const char* name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
int64_t   esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);
""",
    "freertos/FreeRTOS.h": """\
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int          BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t     TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void* QueueHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
typedef void* TimerHandle_t;
typedef void* StreamBufferHandle_t;
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t wait);
""",
    "freertos/task.h": """\
#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void*);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack,
                       void* param, UBaseType_t prio, TaskHandle_t* handle);
void vTaskDelete(TaskHandle_t h);
void vTaskDelay(TickType_t ticks);
""",
    "freertos/timers.h": """\
#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
TimerHandle_t xTimerCreate(const char* name, TickType_t period, BaseType_t reload,
                           void* id, TimerCallbackFunction_t cb);
void* pvTimerGetTimerID(TimerHandle_t t);
BaseType_t xTimerChangePeriod(TimerHandle_t t, TickType_t period, TickType_t wait);
BaseType_t xTimerStart(TimerHandle_t t, TickType_t wait);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t wait);
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t wait);
""",
    "freertos/stream_buffer.h": """\
#pragma once
#include "freertos/FreeRTOS.h"
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger);
size_t xStreamBufferSend(StreamBufferHandle_t h, const void* data, size_t n, TickType_t wait);
size_t xStreamBufferReceive(StreamBufferHandle_t h, void* data, size_t n, TickType_t wait);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t h);
BaseType_t xStreamBufferReset(StreamBufferHandle_t h);
void vStreamBufferDelete(StreamBufferHandle_t h);
""",
    "freertos/semphr.h": """\
#pragma once
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
""",
    "driver/uart.h": """\
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2
#define UART_PIN_NO_CHANGE (-1)
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
typedef enum { UART_DATA, UART_BREAK, UART_EVENT_MAX } uart_event_type_t;
typedef struct { uart_event_type_t type; size_t size; } uart_event_t;
typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    int source_clk;
} uart_config_t;
esp_err_t uart_driver_install(uart_port_t p, int rx_buf, int tx_buf, int q_len,
                              QueueHandle_t* q, int flags);
esp_err_t uart_param_config(uart_port_t p, const uart_config_t* cfg);
esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts);
int  uart_read_bytes(uart_port_t p, void* buf, uint32_t n, TickType_t wait);
int  uart_write_bytes(uart_port_t p, const void* buf, size_t n);
int  uart_write_bytes_with_break(uart_port_t p, const void* buf, size_t n, int brk);
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t wait);
esp_err_t uart_flush_input(uart_port_t p);
esp_err_t uart_set_baudrate(uart_port_t p, uint32_t baud);
esp_err_t uart_driver_delete(uart_port_t p);
""",
}

# ---------------------------------------------------------------------------
# Associate every ```cpp fence with the nearest preceding heading. (The old
# walker required the fence within a few blank lines of the heading, which
# silently skipped any example with an intro paragraph -- e.g. The Slave.)
# ---------------------------------------------------------------------------
lines = readme.splitlines()
blocks = []
hd, title = "#", "untitled"
i = 0
while i < len(lines):
    m = re.match(r"^(#{1,3})\s+(.*)$", lines[i])
    if m:
        hd, title = m.group(1), m.group(2).strip()
        i += 1
        continue
    if lines[i].lstrip().startswith("```cpp"):
        k = i + 1
        while k < len(lines) and not lines[k].lstrip().startswith("```"):
            k += 1
        blocks.append((hd, title, "\n".join(lines[i+1:k])))
        i = k + 1
        continue
    i += 1

if not blocks:
    print("No ```cpp blocks under headings found in README.md", file=sys.stderr)
    sys.exit(1)

failures = []
with tempfile.TemporaryDirectory(prefix="autolink_stubs_") as tmp:
    tmpdir = Path(tmp)
    for rel, content in STUBS.items():
        p = tmpdir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)

    flags = [
        "-std=c++14",
        "-fsyntax-only",
        "-DARDUINO=10607",
        "-I", str(ROOT),
        "-I", str(tmpdir),
    ]

    for hd, title, code in blocks:
        ino = ROOT / f"_readme_{title.replace(' ', '_')}.cpp"
        body = code
        if hd == "###":
            # h3 snippets are excerpts. Declare the `comm` the user's sketch
            # would have and wrap the bare statements so they parse.
            body = f"""
#include "AutoLink.h"
using namespace autolink;
AutoLink comm(UART_NUM_2, 16, 17, true);
Log& __readme_log = Log::getLog();
void __readme_snippet() {{
{code}
}}
"""
        ino.write_text('#include "Arduino.h"\n\n' + body)
        print(f"--- {title} ---")
        r = subprocess.run(["g++", *flags, str(ino)], capture_output=True, text=True)
        if r.returncode == 0:
            print("OK")
            ino.unlink()
        else:
            print("FAIL")
            print(r.stderr)
            failures.append(title)   # keep the file for inspection

if failures:
    print(f"\n{len(failures)} README example(s) failed to compile:")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)
print("\nAll README examples compile.")
