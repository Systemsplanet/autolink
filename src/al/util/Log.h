
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <deque>
#include <string>
#include "al/util/LogRingBuffer.h"

namespace autolink {
class Log {
public:
    enum Level {
        NONE = 0,
        ERROR = 1,
        WARNING = 2,
        INFO = 3,
        DEBUG = 4,
        VERBOSE = 5
    };

    static Log &log() {
        static Log inst;
        return inst;
    }

    void setLevel(Level lv) { lvl = lv; }
    Level getLevel() const { return lvl; }

    bool wouldEmit(Level lvl_for_msg) const { return lvl_for_msg <= lvl; }

    void error(const char *tag, const char *fmt, ...) const {
        if (lvl < ERROR)
            return;
        va_list ap;
        va_start(ap, fmt);
        emit("E", tag, fmt, ap);
        va_end(ap);
    }

    void warning(const char *tag, const char *fmt, ...) const {
        if (lvl < WARNING)
            return;
        va_list ap;
        va_start(ap, fmt);
        emit("W", tag, fmt, ap);
        va_end(ap);
    }

    void info(const char *tag, const char *fmt, ...) const {
        if (lvl < INFO)
            return;
        va_list ap;
        va_start(ap, fmt);
        emit("I", tag, fmt, ap);
        va_end(ap);
    }

    void debug(const char *tag, const char *fmt, ...) const {
        if (lvl < DEBUG)
            return;
        va_list ap;
        va_start(ap, fmt);
        emit("D", tag, fmt, ap);
        va_end(ap);
    }

    void verbose(const char *tag, const char *fmt, ...) const {
        if (lvl < VERBOSE)
            return;
        va_list ap;
        va_start(ap, fmt);
        emit("V", tag, fmt, ap);
        va_end(ap);
    }

    using LogSink = void (*)(char sev, const char *tag, const char *msg,
                             void *ctx);
    void setSink(LogSink fn, void *ctx = nullptr);
    void clearSink();

    // Bounded ring of formatted lines waiting to be written to
    // the host transport (stdout). emit() enqueues — the
    // transport write happens via drainPending(). When the ring
    // is full, the oldest line is dropped (drop-oldest, so the
    // most recent state is preserved on overflow — operators
    // want to see the *current* state, not a backlog of stale
    // lines). A single one-shot "log queue overflow" warning
    // fires the first time a drop happens per overflow streak.
    // Pinned by LogQueueDrainTest.
    //
    // ESP32 uses a SEPARATE fixed-capacity ring (espRing_, no
    // heap allocation) drained by a dedicated low-priority
    // background task instead of this std::deque — see
    // beginEspLogTask(). Both share the same drainPending() entry
    // point and drop-oldest overflow policy; they're separate
    // storage because the host path's std::string entries would
    // be a per-call heap-churn tax with no benefit on-device (see
    // LogQueueDrainTest Pin 6), and the ESP32 path needs a
    // FreeRTOS critical section around push/pop instead of being
    // single-threaded-safe by construction like the host tests.
    void drainPending() const;
    // Number of lines currently in the queue. Test/diagnostics
    // escape hatch.
    size_t pendingCount() const;
    // Drop everything in the queue. Test-only escape hatch.
    void clearPending();

    // Sized so a full burst of state-transition lines (typical
    // OK -> SWP transition: reason + resweep + OK->SWP + P1/P2
    // log) fits without dropping; an ASYNC flood that overruns
    // this still keeps the most recent state visible.
    static constexpr size_t QUEUE_CAP = 64;

    // Spawn the background task that drains espRing_ to ESP_LOGx
    // off the caller's task — emit() only ever does a bounded,
    // heap-free ring-buffer push under a spinlock; the actual
    // UART/console write (which is NOT verified non-blocking —
    // see docs/Version.md) happens on this dedicated low-priority
    // task, never on the caller (timer daemon, uart_event_task,
    // loop task). MUST be called from a scheduler-running context
    // (e.g. EspHal::begin()) — never from a namespace-scope ctor
    // (AGENTS.md rule 17: xTaskCreate before the scheduler starts
    // can crash the kernel). Always declared (not gated behind
    // #ifdef ESP_PLATFORM) so callers like EspHal.cpp — which are
    // only guaranteed ARDUINO is defined, not ESP_PLATFORM, per
    // CompileCheckTest's stub-header syntax check — can call it
    // unconditionally; the body is a no-op on host / when
    // ESP_PLATFORM isn't defined. Idempotent.
    void beginEspLogTask();

private:
    Level lvl = ERROR;

    mutable LogSink sink_fn_ = nullptr;
    mutable void *sink_ctx_ = nullptr;

#ifdef ESP_PLATFORM
    // +1: a ring buffer of capacity N holds N-1 usable entries
    // (one slot reserved to distinguish full from empty — see
    // LogRingBufferTest Pin 2). QUEUE_CAP usable entries needs
    // QUEUE_CAP+1 backing capacity.
    mutable LogRingBuffer<QUEUE_CAP + 1> espRing_;
    mutable bool espDropWarned_ = false;
    bool espTaskStarted_ = false;
#else
    mutable std::deque<std::string> pending_;
    mutable bool dropWarned_ = false;
#endif

    Log() {}
    Log(const Log &) = delete;
    Log &operator=(const Log &) = delete;

    void emit(const char *sev, const char *tag, const char *fmt,
              va_list ap) const;
};

} // namespace autolink
