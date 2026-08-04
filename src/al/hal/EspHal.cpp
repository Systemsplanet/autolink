
#include "al/hal/EspHal.h"
#include "al/hal/EspHalUartEvent.h"
#include "AutoLink.h"

namespace autolink {

void EspHal::begin() {
    if (running)
        return;
    running = true;
    // Started here, not in a ctor (AGENTS.md rule 17): the log
    // drain task's own creation is an RTOS call and must run
    // after the scheduler starts, same reasoning as mutex/
    // task_exit_sem below. Idempotent — safe if begin() is ever
    // called again after a stream_buf allocation failure below
    // returns early.
    Log::log().beginEspLogTask();
    if (!mutex)
        mutex = xSemaphoreCreateMutex();
    if (!task_exit_sem)
        task_exit_sem = xSemaphoreCreateBinary();

    if (rx_buffer_size_ == 0)
        rx_buffer_size_ = rxBufferFloor(cfg);
    if (tx_buffer_size_ == 0)
        tx_buffer_size_ = txBufferFloor(cfg);
    stream_buf_size_ = streamBufferFloor(cfg);

    {
        size_t freeH = (size_t)esp_get_free_heap_size();
        size_t sbMin = (size_t)cfg.maxMsg + 6;
        size_t sbWas = stream_buf_size_;
        stream_buf_size_ = capFloorByHeap(stream_buf_size_, sbMin, freeH,
                                          cfg.heapReserveBytes);
        freeH = freeH > stream_buf_size_ ? freeH - stream_buf_size_ : 0;
        size_t rxWas = rx_buffer_size_;
        rx_buffer_size_ = capFloorByHeap(rx_buffer_size_, cfg.rxBufferSize,
                                         freeH, cfg.heapReserveBytes);
        if (stream_buf_size_ != sbWas || rx_buffer_size_ != rxWas)
            Log::log().warning(
                TAG, "heap cap: streamBuf %u->%u rxBuf %u->%u (reserve=%u)",
                (unsigned)sbWas, (unsigned)stream_buf_size_, (unsigned)rxWas,
                (unsigned)rx_buffer_size_, (unsigned)cfg.heapReserveBytes);
    }
    if (!stream_buf) {
        stream_buf = xStreamBufferCreate(stream_buf_size_, 1);
        if (!stream_buf) {
            Log::log().error(
                TAG,
                "xStreamBufferCreate"
                " failed (%uB) — aborting begin(), link stays down",
                (unsigned)stream_buf_size_);
            if (mutex) {
                vSemaphoreDelete(mutex);
                mutex = nullptr;
            }
            if (task_exit_sem) {
                vSemaphoreDelete(task_exit_sem);
                task_exit_sem = nullptr;
            }
            running = false;
            return;
        }
    }
    Log::log().info(TAG,
                    "begin: version=" AUTOLINK_VERSION " free heap=%u"
                    " rxBuf=%u txBuf=%u streamBuf=%u"
                    " mode=%s maxMsg=%u",
                    (unsigned)esp_get_free_heap_size(),
                    (unsigned)rx_buffer_size_, (unsigned)tx_buffer_size_,
                    (unsigned)stream_buf_size_,
                    cfg.mode == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
                    (unsigned)cfg.maxMsg);
    auto cleanup = [&]() {
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

    if (uart_is_driver_installed(uart_num)) {
        uart_driver_delete(uart_num);
    }
    esp_err_t e = uart_driver_install(uart_num, rx_buffer_size_,
                                      tx_buffer_size_, 10, &uart_queue, 0);
    if (e == ESP_ERR_NO_MEM) {
        Log::log().error(TAG,
                         "uart_driver_install OOM:"
                         " free heap=%u",
                         (unsigned)esp_get_free_heap_size());
        cleanup();
        return;
    }
    if (e != ESP_OK) {
        Log::log().error(TAG, "uart_driver_install: %s", esp_err_to_name(e));
        cleanup();
        return;
    }
    e = uart_param_config(uart_num, &uart_config);
    if (e != ESP_OK) {
        Log::log().error(TAG, "uart_param_config: %s", esp_err_to_name(e));
        uart_driver_delete(uart_num);
        cleanup();
        return;
    }
    e = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        Log::log().error(TAG,
                         "uart_set_pin tx=%d rx=%d"
                         ": %s",
                         tx_pin, rx_pin, esp_err_to_name(e));
        uart_driver_delete(uart_num);
        cleanup();
        return;
    }
    Log::log().info(TAG, "UART%d tx=GPIO%d rx=GPIO%d", (int)uart_num, tx_pin,
                    rx_pin);
    esp_err_t gp = gpio_set_pull_mode((gpio_num_t)rx_pin, GPIO_PULLUP_ONLY);
    if (gp != ESP_OK)
        Log::log().error(TAG, "pull_mode GPIO%d: %s", rx_pin,
                         esp_err_to_name(gp));
    if (xTaskCreatePinnedToCore(uart_event_task, "uart_ev_task", 4096, this, 5,
                                &task_handle, 1) != pdPASS) {
        Log::log().error(TAG, "UART task create failed");
        uart_driver_delete(uart_num);
        cleanup();
        return;
    }
    timer_handle = xTimerCreate("alink_tmr", pdMS_TO_TICKS(50), pdFALSE, this,
                                timer_callback);
    if (!timer_handle) {
        Log::log().error(TAG, "timer create failed");
        uart_driver_delete(uart_num);
        cleanup();
        return;
    }
    healthy = true;
}

EspHal::~EspHal() {
    running = false;
    if (task_handle && task_exit_sem)
        xSemaphoreTake(task_exit_sem, portMAX_DELAY);
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

} // namespace autolink
