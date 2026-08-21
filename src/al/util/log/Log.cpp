#include "al/util/log/Log.h"
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
    // Pinned to core 0 (PRO_CPU, the protocol core on
    // ESP32-D0WD). The Arduino loop task lives on core 1
    // (APP_CPU); with the drain task un-pinned and both at
    // similar priority, a burst of log lines enqueued by
    // protocol tasks on core 0 could sit unread while core 1
    // stayed busy, because the scheduler had no reason to
    // break the loop task away just to run a same-priority
    // drain on the other core. Pinning to core 0 decouples
    // the two schedules: core 0 runs the drain on every tick
    // regardless of what core 1 is doing. Field-soak showed a
    // 17 s stretch of zero output while the peer log streamed
    // 68 echoes — the same-core-starvation shape. Pinned by
    // LogDrainStarvedOnSameCoreTest (source-grep; task
    // pinning is not runtime-observable on host).
    if (xTaskCreatePinnedToCore(logDrainTaskFn, "alink_log", 3072, nullptr, 1,
                                nullptr, 0) != pdPASS) {
        // Not fatal: emit() still enqueues into espRing_ (bounded,
        // drop-oldest), it just never drains without this task —
        // log lines stop reaching the console/dashboard but the
        // link itself keeps working. Route through the sink
        // directly if one is set (not through Log::error(), which
        // re-enters emit() and would recurse through this exact
        // failure). No fprintf(stderr, ...) here per AGENTS rule
        // 10 — stderr is a no-op on most ESP32 console configs, so
        // the prior fprintf silently discarded this warning
        // on-device. If no sink is set, the failure has no
        // reachable surface and is dropped, same as any other
        // early-boot log with no sink attached yet.
        if (sink_fn_) {
            sink_fn_('E', "AutoLink",
                     "beginEspLogTask: task create failed — log "
                     "lines will queue but never drain",
                     sink_ctx_);
        }
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

    // Sized from LogRingBuffer::Entry::msg (400 bytes), not a
    // hardcoded literal. The prior 384 was 16 bytes short of the
    // ring entry, so every [A]/[S] stats line that formatted to
    // more than 384 bytes was silently clipped mid-token
    // (`...staleAmbig=0  ack` / `ac` / `a`, varying with the
    // width of the echos= counter). Deriving the size from the
    // ring entry's own constant means the two can't drift apart
    // again. Runtime-pinned by LogTest.cpp's
    // test_long_message_truncated_at_buffer (asserts the
    // captured line lands in (384, 400] bytes, not <= 384).
    constexpr size_t kMsgBufBytes = sizeof(LogRingBuffer<2>::Entry::msg);
    char msg[kMsgBufBytes];
    int needed = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (needed > (int)sizeof(msg) - 1) {
        // Routed through the sink directly, not fprintf(stderr, ...)
        // (AGENTS rule 10: stderr is a no-op on most ESP32 console
        // configs, so the prior fprintf silently discarded this
        // warning on-device). Can't call Log::error()/warning()
        // here — those call emit(), and this branch is inside
        // emit(), so that would recurse. sink_fn_ is called
        // directly instead, same as the truncated line itself is
        // about to be. Latched process-wide (not per-tag) so a
        // storm of differently-shaped oversized lines doesn't
        // spam the sink once the condition is known. Pinned by
        // LogTruncationRoutedThroughSinkTest.
        static bool truncWarned_ = false;
        if (!truncWarned_) {
            truncWarned_ = true;
            if (sink_fn_) {
                char wbuf[160];
                int wn = snprintf(wbuf, sizeof(wbuf),
                                  "Log::emit: line truncated (needed %d bytes, "
                                  "buffer %u). Shorten the format string or "
                                  "raise the buffer size.",
                                  needed, (unsigned)sizeof(msg));
                if (wn > 0)
                    sink_fn_('E', tag, wbuf, sink_ctx_);
            }
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
    //
    // esp_log_timestamp() is captured HERE, at push time, not left
    // for ESP_LOGx to stamp when the drain task eventually gets to
    // this entry. ESP_LOGx's own (nnnnn) prefix reflects whenever
    // the drain task actually calls it — if the queue backs up
    // (a burst under load, or the drain task starved — see the
    // Pong not-ready path fix), consecutive printed lines can carry
    // near-identical (nnnnn) values while seconds of real elapsed
    // time separate the events they describe, and any duration an
    // operator reads off two log lines is fiction. The captured
    // value is prefixed onto the message so the printed line reads
    // "[@nnnnn] I (mmmmm) ...", with the true producer-time value
    // first; a growing gap between the two numbers is itself the
    // backlog signal. Pinned by LogTimestampCaptureTest.
    uint32_t tsMs = (uint32_t)esp_log_timestamp();
    bool dropped;
    bool warnNow = false;
    bool clearWarn = false;
    portENTER_CRITICAL(&gLogMux);
    dropped = espRing_.tryPush(sev[0], tag, msg, tsMs);
    // AL90-8: bump the drop counter
    // INSIDE the same critical section the
    // ring push holds. emit() is called
    // from the loop task, the uart_event
    // task, and the timer daemon — the
    // previous "bump here, just past
    // portEXIT" placement left a window
    // where two producers each saw a
    // dropped=true ring push, both
    // returned to user code, and one of
    // the ++ operations was lost. The
    // espDropWarned_ flag is also accessed
    // outside the lock on the
    // not-dropped path, so that path is
    // moved inside as well. Pinned by
    // LogDropsCounterRaceTest.
    if (dropped) {
        droppedLines_++;
        if (!espDropWarned_) {
            espDropWarned_ = true;
            warnNow = true;
        }
    } else {
        espDropWarned_ = false;
        clearWarn = true;
    }
    portEXIT_CRITICAL(&gLogMux);
    if (warnNow) {
        // Routed through the sink, not fputs(stderr, ...) — see
        // the rationale on the truncation warning above; this
        // call sees msg/sink_fn_ already in scope from this
        // same emit() invocation, so it costs nothing extra.
        if (sink_fn_) {
            sink_fn_('W', "AutoLink",
                     "Log: ESP ring overflow, dropping oldest "
                     "lines (drain task not keeping up)",
                     sink_ctx_);
        }
    }
    (void)clearWarn;
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
            // AL89-11: persistent counter
            // bumped on every drop, not just
            // on the first drop of a
            // streak. Bumped here, before the
            // drop-warning path, so the
            // counter increments regardless
            // of whether the warning was
            // latched.
            droppedLines_++;
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
            ESP_LOGE(e.tag, "[@%lu] %s", (unsigned long)e.tsMs, e.msg);
            break;
        case 'W':
            ESP_LOGW(e.tag, "[@%lu] %s", (unsigned long)e.tsMs, e.msg);
            break;
        case 'V':
            ESP_LOGV(e.tag, "[@%lu] %s", (unsigned long)e.tsMs, e.msg);
            break;
        case 'D':
            ESP_LOGD(e.tag, "[@%lu] %s", (unsigned long)e.tsMs, e.msg);
            break;
        default:
            ESP_LOGI(e.tag, "[@%lu] %s", (unsigned long)e.tsMs, e.msg);
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

uint64_t Log::droppedLines() const { return droppedLines_; }

void Log::clearDroppedLines() { droppedLines_ = 0; }

} // namespace autolink
