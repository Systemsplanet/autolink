#include "al/util/Log.h"
#include <cstdlib>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#    include "esp_log.h"
#    include "freertos/FreeRTOS.h"
#    include "freertos/task.h"
#endif

namespace autolink {

#ifdef ESP_PLATFORM

namespace {
// Statically initialized (no RTOS call) — safe to use even if
// emit() is somehow reached before the scheduler starts. Never
// held across an ESP_LOGx call: entries are copied out of
// espRing_ under the lock, then dispatched after releasing it, so
// a slow UART write never extends how long interrupts are
// disabled on this core.
portMUX_TYPE gLogMux = portMUX_INITIALIZER_UNLOCKED;

void logDrainTaskFn(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        Log::log().drainPending();
    }
}
} // namespace

void Log::beginEspLogTask() {
    if (espTaskStarted_)
        return;
    espTaskStarted_ = true;
    // Priority 1: just above idle, below every protocol-handling
    // task (uart_event_task=5, the FreeRTOS timer daemon's default
    // priority is higher still). If ESP_LOGx blocks on a slow
    // console UART, it blocks this task alone — never the timer
    // daemon, never uart_event_task, never the caller of
    // Log::info()/warning()/etc.
    if (xTaskCreatePinnedToCore(logDrainTaskFn, "alink_log", 3072, nullptr, 1,
                                nullptr, tskNO_AFFINITY) != pdPASS) {
        // Not fatal: emit() still enqueues into espRing_ (bounded,
        // drop-oldest), it just never drains without this task —
        // log lines stop reaching the console/dashboard but the
        // link itself keeps working. Logged via fprintf, not
        // Log::, since Log:: is exactly what's failing to drain
        // here.
        fprintf(stderr,
                "E [AutoLink] beginEspLogTask: task create "
                "failed — log lines will queue but never "
                "drain\n");
    }
}

#endif // ESP_PLATFORM

#ifndef ESP_PLATFORM
namespace {
// At-exit drain so test binaries flush the pending queue before
// stdio cleanup runs. Without this the queued lines would never
// reach stdout when the test binary exits via main() return
// (atexit runs after main, but before the C runtime tears down
// stdio). Host-only: ESP32 drains via the dedicated background
// task instead (see beginEspLogTask()).
void atexitDrain() { Log::log().drainPending(); }
} // namespace

// No-op on host / when ESP_PLATFORM isn't defined — there is no
// background task to start, drainPending() already runs
// synchronously via the atexit hook above. Always defined (not
// gated) so callers that can't guarantee ESP_PLATFORM is defined
// (EspHal.cpp only relies on ARDUINO — see the declaration in
// Log.h) can call it unconditionally.
void Log::beginEspLogTask() {}
#endif

void Log::setSink(LogSink fn, void *ctx) {
    sink_fn_ = fn;
    sink_ctx_ = ctx;
}

void Log::clearSink() {
    sink_fn_ = nullptr;
    sink_ctx_ = nullptr;
}

void Log::emit(const char *sev, const char *tag, const char *fmt,
               va_list ap) const {
    if (lvl == NONE)
        return;

    char msg[384];
    int needed = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (needed > (int)sizeof(msg) - 1) {
        static bool truncWarned_ = false;
        if (!truncWarned_) {
            fprintf(
                stderr,
                "E [%s] Log::emit: line truncated (needed %d bytes, buffer %u). Shorten the format string or raise the buffer size.\n",
                tag, needed, (unsigned)sizeof(msg));
            truncWarned_ = true;
        }
    }

    if (sink_fn_)
        sink_fn_(sev[0], tag, msg, sink_ctx_);

#ifdef ESP_PLATFORM
    // Bounded, heap-free push under a spinlock — never the slow
    // part. The actual ESP_LOGx dispatch (not verified
    // non-blocking on this project's console transport — see
    // docs/Version.md) happens later, off this caller's task, via
    // the background task started by beginEspLogTask(). If that
    // task hasn't been started yet (e.g. a log call during early
    // boot before EspHal::begin() runs), lines simply queue until
    // it is — bounded by QUEUE_CAP, drop-oldest on overflow, same
    // as every other overflow case.
    bool dropped;
    portENTER_CRITICAL(&gLogMux);
    dropped = espRing_.tryPush(sev[0], tag, msg);
    portEXIT_CRITICAL(&gLogMux);
    if (dropped) {
        if (!espDropWarned_) {
            espDropWarned_ = true;
            static const char kDropMsg[] =
                "W [AutoLink] Log: ESP ring overflow, dropping oldest "
                "lines (drain task not keeping up)\n";
            fputs(kDropMsg, stderr);
        }
    } else {
        espDropWarned_ = false;
    }
#else
    // Host-only: enqueue for the deferred stdout write done by
    // drainPending() instead of writing inline, so emit() never
    // blocks on a slow flush of the host stream. Pinned by
    // LogQueueDrainTest.
    char queued[448];
    int qn = snprintf(queued, sizeof(queued), "%c [%s] %s", sev[0], tag, msg);
    if (qn > 0 && qn < (int)sizeof(queued)) {
        if (pending_.size() >= QUEUE_CAP) {
            // Drop-oldest so the most recent state is
            // preserved on overflow.
            pending_.pop_front();
            if (!dropWarned_) {
                dropWarned_ = true;
                static const char kDropMsg[] =
                    "W [AutoLink] Log: queue overflow, dropping oldest "
                    "lines (drainPending not called often enough)\n";
                // Direct stderr write for the overflow
                // warning — this line itself is best-effort
                // and must not recursively enqueue.
                fputs(kDropMsg, stderr);
            }
        }
        pending_.push_back(std::string(queued));
        // Reset the drop warning latch on a successful
        // enqueue so a future overflow streak gets its own
        // warning.
        dropWarned_ = false;
    }

    // First-emit hookup of the atexit drain. Done here (not in
    // the ctor) so the handler doesn't fire when the process
    // never produced a log line, and so the singleton's static
    // init ordering doesn't depend on atexit registration.
    static bool atexitRegistered = false;
    if (!atexitRegistered) {
        atexitRegistered = true;
        std::atexit(atexitDrain);
    }
#endif
}

void Log::drainPending() const {
#ifdef ESP_PLATFORM
    // Copy each entry out under the lock, dispatch after
    // releasing it — a slow ESP_LOGx write must never happen
    // while interrupts are disabled on this core. Runs on the
    // dedicated background task (see beginEspLogTask()), so a
    // slow write here costs that task alone, not the caller.
    for (;;) {
        LogRingBuffer<QUEUE_CAP + 1>::Entry e;
        bool got;
        portENTER_CRITICAL(&gLogMux);
        got = espRing_.tryPop(&e);
        portEXIT_CRITICAL(&gLogMux);
        if (!got)
            break;
        switch (e.sev) {
        case 'E':
            ESP_LOGE(e.tag, "%s", e.msg);
            break;
        case 'W':
            ESP_LOGW(e.tag, "%s", e.msg);
            break;
        case 'V':
            ESP_LOGV(e.tag, "%s", e.msg);
            break;
        case 'D':
            ESP_LOGD(e.tag, "%s", e.msg);
            break;
        default:
            ESP_LOGI(e.tag, "%s", e.msg);
            break;
        }
    }
#else
    if (pending_.empty())
        return;
    // Pop and write each queued line to stdout. This is the
    // only path that calls fflush on the host — a slow UART
    // here blocks the test harness / operator console, but
    // it does not block emit() (the actual on-the-wire
    // paths).
    while (!pending_.empty()) {
        std::string line = pending_.front();
        pending_.pop_front();
        fwrite(line.data(), 1, line.size(), stdout);
        fputc('\n', stdout);
    }
    fflush(stdout);
#endif
}

size_t Log::pendingCount() const {
#ifdef ESP_PLATFORM
    size_t n;
    portENTER_CRITICAL(&gLogMux);
    n = espRing_.size();
    portEXIT_CRITICAL(&gLogMux);
    return n;
#else
    return pending_.size();
#endif
}

void Log::clearPending() {
#ifdef ESP_PLATFORM
    portENTER_CRITICAL(&gLogMux);
    LogRingBuffer<QUEUE_CAP + 1>::Entry e;
    while (espRing_.tryPop(&e)) {
    }
    portEXIT_CRITICAL(&gLogMux);
#else
    pending_.clear();
#endif
}

} // namespace autolink
