import os

STUBS = {
    "driver/uart.h": """#pragma once
#include <Arduino.h>
typedef int uart_port_t;
struct uart_event_t { int type; int size; };
typedef int TickType_t;
struct uart_config_t {
  int baud_rate; int data_bits; int parity; int stop_bits;
  int flow_ctrl; int rx_flow_ctrl_thresh; int source_clk;
};
inline int uart_driver_install(int, int, int, int, void *, int) { return 0; }
inline int uart_driver_delete(int) { return 0; }
inline bool uart_is_driver_installed(int) { return false; }
inline int uart_param_config(int, const uart_config_t *) { return 0; }
inline int uart_set_pin(int, int, int, int, int) { return 0; }
inline int uart_set_baudrate(int, int) { return 0; }
inline int uart_flush_input(int) { return 0; }
inline int uart_write_bytes(int, const char *, int) { return 0; }
inline int uart_write_bytes_with_break(int, const char *, int, int) { return 0; }
inline int uart_read_bytes(int, void *, int, int) { return 0; }
inline int uart_wait_tx_done(int, int) { return 0; }
#define UART_DATA 1
#define UART_BREAK 2
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 0
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_APB 0
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2
#define UART_PIN_NO_CHANGE -1
#define portMAX_DELAY 0xffffffff
inline int xQueueReceive(void *, void *, int) { return 0; }
inline void *pvTimerGetTimerID(void *) { return 0; }
""",
    "driver/gpio.h": """#pragma once
typedef int gpio_num_t;
#define GPIO_PULLUP_ONLY 1
inline int gpio_set_pull_mode(int, int) { return 0; }
""",
    "esp_err.h": "#pragma once\n",
    # esp_timer.h: define struct as forward-decl so Arduino.h's
    # esp_timer_create_args_t is not redefined
    "esp_timer.h": """#pragma once
#include <Arduino.h>
typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);
struct esp_timer_create_args_t {
    esp_timer_cb_t callback;
    void *arg;
    unsigned int dispatch_method;
    const char *name;
    bool skip_unhandled_events;
};
#define ESP_TIMER_TASK 0
inline long long esp_timer_get_time() { return 0; }
inline int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *) { return 0; }
inline int esp_timer_start_periodic(void *, unsigned long long) { return 0; }
inline int esp_timer_start_once(void *, unsigned long long) { return 0; }
inline int esp_timer_stop(void *) { return 0; }
inline int esp_timer_delete(void *) { return 0; }
inline unsigned long long esp_get_free_heap_size() { return 0; }
inline void configTime(long, int, const char *, const char *) {}
inline void configTime(long, int, const char *, const char *, const char *) {}
inline bool getLocalTime(struct tm *, int = 0) { return false; }
""",
    "esp_heap_caps.h": "#pragma once\ninline int heap_caps_get_free_size(unsigned) { return 0; }\n",
    "esp_system.h": "#pragma once\ninline void esp_restart() {}\ninline unsigned int esp_random() { return 0; }\n",
    "freertos/FreeRTOS.h": """#pragma once
#include <algorithm>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef int TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *StreamBufferHandle_t;
typedef void *TimerHandle_t;
inline BaseType_t xPortGetCoreID() { return 0; }
inline UBaseType_t uxTaskPriorityGet(void *) { return 0; }
inline void vTaskDelete(void *) {}
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, int, TaskHandle_t *, int) { return 1; }
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, int, TaskHandle_t *) { return 1; }
inline StreamBufferHandle_t xStreamBufferCreate(size_t, size_t) { return 0; }
inline void *xSemaphoreCreateBinary() { return 0; }
inline void vStreamBufferDelete(void *) {}
inline size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t, unsigned) { return 0; }
inline size_t xStreamBufferReceive(StreamBufferHandle_t, void *, size_t, unsigned) { return 0; }
inline size_t xStreamBufferBytesAvailable(StreamBufferHandle_t) { return 0; }
inline int xStreamBufferReset(StreamBufferHandle_t) { return 0; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return 0; }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return 0; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline void vTaskDelay(unsigned) {}
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
inline TimerHandle_t xTimerCreate(const char *, int, int, void *, void (*)(void *)) { return 0; }
inline int xTimerDelete(TimerHandle_t, int) { return 0; }
inline int xTimerChangePeriod(TimerHandle_t, int, int) { return 0; }
inline int xTimerStart(TimerHandle_t, int) { return 0; }
inline int xTimerStop(TimerHandle_t, int) { return 0; }
inline unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
inline void portYIELD() {}
""",
    "freertos/semphr.h": "#pragma once\n",
    "freertos/stream_buffer.h": "#pragma once\n",
    "freertos/task.h": "#pragma once\n",
    "freertos/timers.h": "#pragma once\n",
    "Stream.h": "#pragma once\n#include <Arduino.h>\n",
    "Print.h": "#pragma once\n#include <Arduino.h>\n",
    "Preferences.h": """#pragma once
#include <Arduino.h>
#include <WString.h>
class Preferences { public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  int getInt(const char *, int = 0) { return 0; }
  void putInt(const char *, int) {}
  unsigned char getUChar(const char *, unsigned char = 0) { return 0; }
  int putUChar(const char *, unsigned char) { return 0; }
  String getString(const char *) { return ""; }
  void putString(const char *, const String &) {}
};
""",
    "WiFi.h": """#pragma once
#include <Arduino.h>
#include <WString.h>
#define WIFI_STA 1
#define WL_CONNECTED 3
struct IPAddress { String toString() const { return ""; } };
class WiFiClass { public:
  int RSSI() { return 0; }
  int status() { return 0; }
  bool isConnected() { return false; }
  IPAddress localIP() { return IPAddress(); }
  void mode(int) {}
  void begin(const char *, const char *) {}
  void disconnect() {}
  void stop() {}
};
extern WiFiClass WiFi;
""",
    "esp_event.h": "#pragma once\n",
    "esp_http_server.h": """#pragma once
#include <Arduino.h>
typedef void *httpd_handle_t;
struct httpd_req {
  void *user_ctx;
};
typedef struct httpd_req httpd_req_t;
struct httpd_config_t {
  int server_port;
  int stack_size;
  int task_priority;
  int max_open_sockets;
  int lru_purge_enable;
  int max_uri_handlers;
};
struct httpd_uri_t {
  const char *uri;
  int method;
  int (*handler)(httpd_req_t *);
  void *user_ctx;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
#define HTTP_GET 1
#define HTTP_POST 2
inline int httpd_start(httpd_handle_t *, const httpd_config_t *) { return 0; }
inline int httpd_stop(httpd_handle_t) { return 0; }
inline int httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *) { return 0; }
inline int httpd_req_get_url_query_str(httpd_req_t *, char *, size_t) { return 0; }
inline int httpd_query_key_value(const char *, const char *, char *, size_t) { return 0; }
inline int httpd_resp_set_status(httpd_req_t *, const char *) { return 0; }
inline int httpd_resp_set_type(httpd_req_t *, const char *) { return 0; }
inline int httpd_resp_send(httpd_req_t *, const char *, size_t) { return 0; }
inline int httpd_resp_send_chunk(httpd_req_t *, const char *, size_t) { return 0; }
inline int httpd_resp_sendstr_chunk(httpd_req_t *, const char *) { return 0; }
inline int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) { return 0; }
""",
    "WString.h": """#pragma once
class String { public:
  String() {}
  String(const char *) {}
  String(int) {}
  const char *c_str() const { return ""; }
  int length() const { return 0; }
  int toInt() const { return 0; }
  bool operator==(const String &) const { return false; }
  String &operator=(const char *) { return *this; }
  String &operator+=(const String &) { return *this; }
  operator const char *() const { return ""; }
};
""",
}

base = "/tmp/include"
os.makedirs(base, exist_ok=True)
for path, content in STUBS.items():
    full = os.path.join(base, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w") as f:
        f.write(content)
print("done")
