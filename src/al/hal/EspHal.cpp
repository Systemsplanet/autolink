
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

    auto cleanup = [&]() {
        running = false;
        // Zero the ring sizes so txRingSize() cannot report a
        // figure that was never installed; Link::begin() treats
        // 0 as "unknown" and falls back to config math.
        tx_buffer_size_ = 0;
        rx_buffer_size_ = 0;
        stream_buf_size_ = 0;
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

    // Single reservation for the rest of the system (LWIP, httpd, WiFi),
    // taken once from the total, then distributed across the three
    // buffers from what's left. Calling capFloorByHeap
    // three times, passing cfg.heapReserveBytes as the reserve at EVERY
    // call against the already-shrunk running counter — once any earlier
    // step clamped, the counter converged to exactly `reserve`, and
    // whichever buffer was sized last (tx) then computed
    // avail = reserve - reserve = 0 regardless of how much heap was
    // actually still free. distributeHeapBudget() (AutoLinkConfig.h)
    // takes the reserve once up front and distributes the remainder, so
    // only genuine insufficiency produces a 0, not step order.
    // EspHalHeapAccountingTest calls the same function this line does.
    size_t totalFreeH = (size_t)esp_get_free_heap_size();
    size_t sbWas = stream_buf_size_;
    size_t rxWas = rx_buffer_size_;
    size_t txWas = tx_buffer_size_;
    HeapDistribution dist = distributeHeapBudget(cfg, totalFreeH);
    stream_buf_size_ = dist.streamBuf;
    rx_buffer_size_ = dist.rxBuf;
    tx_buffer_size_ = dist.txBuf;
    size_t freeH = dist.postFree;
    if (stream_buf_size_ != sbWas || rx_buffer_size_ != rxWas ||
        tx_buffer_size_ != txWas)
        Log::log().warning(
            TAG,
            "heap cap: streamBuf %u->%u rxBuf %u->%u txBuf %u->%u "
            "(reserve=%u)",
            (unsigned)sbWas, (unsigned)stream_buf_size_, (unsigned)rxWas,
            (unsigned)rx_buffer_size_, (unsigned)txWas,
            (unsigned)tx_buffer_size_, (unsigned)cfg.heapReserveBytes);

    // takeFromBudget() above returns 0 for any buffer the remaining
    // budget can't cover even at its floor. Catch it before any
    // allocation: a 0 stream buffer violates the FreeRTOS size > 0
    // precondition and trips configASSERT, and a 0 rx ring makes the
    // driver install return ESP_ERR_INVALID_ARG, which reads as a
    // driver fault rather than the OOM it is. Pinned by
    // HeapStarvedFloorsReportZeroTest.
    if (stream_buf_size_ == 0 || rx_buffer_size_ == 0 || tx_buffer_size_ == 0) {
        Log::log().error(TAG,
                         "insufficient heap: streamBuf=%u rxBuf=%u txBuf=%u"
                         " (free=%u reserve=%u) — aborting begin(), "
                         "link stays down",
                         (unsigned)stream_buf_size_, (unsigned)rx_buffer_size_,
                         (unsigned)tx_buffer_size_, (unsigned)totalFreeH,
                         (unsigned)cfg.heapReserveBytes);
        cleanup();
        return;
    }
    if (!stream_buf) {
        stream_buf = xStreamBufferCreate(stream_buf_size_, 1);
        if (!stream_buf) {
            Log::log().error(TAG,
                             "xStreamBufferCreate failed (%uB) — "
                             "aborting begin(), link stays down",
                             (unsigned)stream_buf_size_);
            cleanup();
            return;
        }
    }
    // Post-allocation floor. LWIP at the default LWIP_MAX_SOCKETS=10 +
    // httpd's max_open_sockets needs ~20-30 KB after our allocations;
    // below that the accept() / mbuf paths start failing at runtime
    // with no clear diagnostic. Fail loudly here instead. 20 KB sits
    // above the reserve guarantee (post-alloc free is budget-derived,
    // not a fixed subtraction — see the accounting above — so a value
    // at or below the reserve is a real shortfall, not an artifact).
    // Pinned by EspHalHeapAccountingTest.
    constexpr size_t kServiceableFloor = 20 * 1024;
    if (freeH < kServiceableFloor) {
        Log::log().error(
            TAG,
            "post-allocation free heap %u below serviceable floor %u — "
            "aborting begin(), link stays down (httpd / WiFi likely to "
            "fail at socket-accept). "
            "Lower cfg.maxMsg or raise the device's free heap.",
            (unsigned)freeH, (unsigned)kServiceableFloor);
        cleanup();
        return;
    }
    Log::log().info(TAG,
                    "begin: version=" AUTOLINK_VERSION " free heap=%u"
                    " post-alloc free=%u"
                    " rxBuf=%u txBuf=%u streamBuf=%u"
                    " mode=%s maxMsg=%u",
                    (unsigned)totalFreeH, (unsigned)freeH,
                    (unsigned)rx_buffer_size_, (unsigned)tx_buffer_size_,
                    (unsigned)stream_buf_size_,
                    cfg.mode == AutoLinkConfig::Mode::ASYNC ? "ASYNC" : "SYNC",
                    (unsigned)cfg.maxMsg);
    if (uart_is_driver_installed(uart_num)) {
        uart_driver_delete(uart_num);
    }
    esp_err_t e =
        uart_driver_install(uart_num, rx_buffer_size_, tx_buffer_size_, 64,
                            &uart_queue, ESP_INTR_FLAG_IRAM);
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
    // AL89-3: pin to core 0 (PRO_CPU). The previous
    // shape pinned to core 1 — the same core as the
    // Arduino loopTask — and at sustained 512000-baud
    // inbound the uart_event_task's tight onRx loop
    // starved the loopTask of CPU. The app task
    // never ran for 8.24 s during the field capture,
    // 84 messages piled up in the 4108 B pong stream
    // buffer, every 16 ms the master saw "app buf
    // full" NAKs and answered with a full-window
    // resend (the dedup window was 8 ms — see
    // AL89-6), the peer-reset watchdog tripped, the
    // slave camped at 512000 while the master walked
    // P1. 40.6 s of a 62 s run lost. Core 0 is the
    // same pinning already used for the log drain
    // task (Log.cpp:58) for the identical starvation
    // shape. Single-core ESP32-C3/-S2 fall back to
    // an un-pinned task; a throttled vTaskDelay(1)
    // inside uart_event_task (see EspHalUartEvent.h,
    // immediately after the onRx call — a plain
    // taskYIELD() cannot schedule a lower-priority
    // task away from this one, AL92-2) bounds the
    // same starvation there. Pinned by
    // UartEvTaskPinnedToProtocolCoreTest.
    int evCore = 0;
    // AL90-4: single-core detection.
    // `#if configNUM_CORES < 2` was
    // unreliable — on Arduino-ESP32 against
    // IDF 4.4, configNUM_CORES can be
    // undefined (an undefined identifier in
    // #if is 0, so 0 < 2 silently fires and
    // evCore becomes tskNO_AFFINITY on the
    // dual-core ESP32 this pin targets). The
    // three macros below are the real
    // single-core signals across FreeRTOS
    // ports: CONFIG_FREERTOS_UNICORE (the
    // IDF 5.x Kconfig), portNUM_PROCESSORS
    // (the pre-SMP non-Kconfig port), and
    // configNUM_CORES (the SMP port with
    // single-core build).
    // AL92-1: fixed a typo — this guard read
    // CONFIG_FREERTOS_UNICODE (not a real
    // FreeRTOS/IDF macro; grep confirms it is
    // defined nowhere in this tree) instead of
    // CONFIG_FREERTOS_UNICORE, the real IDF 5.x
    // Kconfig symbol. An always-false first
    // disjunct is harmless as long as the other
    // two disjuncts are sufficient, but it means
    // this guard has never actually caught the
    // IDF 5.x single-core signal it names.
#if defined(CONFIG_FREERTOS_UNICORE) ||                        \
    (defined(portNUM_PROCESSORS) && portNUM_PROCESSORS < 2) || \
    (defined(configNUM_CORES) && configNUM_CORES < 2)
    evCore = tskNO_AFFINITY;
#endif
    singleCoreEvTask_ = (evCore == tskNO_AFFINITY);
    if (xTaskCreatePinnedToCore(uart_event_task, "uart_ev_task", 4096, this, 5,
                                &task_handle, evCore) != pdPASS) {
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
