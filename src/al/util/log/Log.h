
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <deque>
#include <string>
#include "al/util/log/LogRingBuffer.h"

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
    // AL89-11: persistent count of lines
    // dropped on overflow. Reset by
    // clearDroppedLines() (test hook). The
    // field capture's 42.8 s hole was
    // inferable from a gap in the log but
    // not countable; this surfaces it as a
    // permanent diagnostic so a saturated
    // ring is detected without grepping
    // timestamps.
    uint64_t droppedLines() const;
    void clearDroppedLines();
    // Drop everything in the queue. Test-only escape hatch.
    void clearPending();

    // Sized so a full burst of state-transition lines (typical
    // OK -> SWP transition: reason + resweep + OK->SWP + P1/P2
    // log) fits without dropping; an ASYNC flood that overruns
    // this still keeps the most recent state visible.
    //
    // AL89-11: bumped from 64 to 256. The 64-line
    // ring lost 42.8 s of master log on the field
    // capture's failure path — a saturated retx
    // storm at ASYNC pipeline rate overran the
    // ring inside one second and the per-frame
    // retx warning ate the ring slot faster than
    // the drain task could flush. The 256 cap
    // covers the same state-transition burst with
    // 4× the headroom, comfortably absorbing a
    // one-second saturation at 250 lines/s
    // (the retx debug demote removes the
    // per-frame chatter — see LinkTimersOk.cpp).
    // Memory cost: 256 × 400 B ≈ 100 KB on the
    // ring alone, allocated out of the heap
    // budget (the host path uses std::deque
    // entries of unbounded size — see Log.cpp).
    // The cap is also exposed via the
    // droppedLines_ counter (Stats) so a
    // saturated ring is countable, not just
    // inferable from a hole in the log.
    static constexpr size_t QUEUE_CAP = 128;
    // AL94-1: this budget was wrong. The
    // static_assert below previously bounded
    // the ring against an assumed "~60 KB free
    // heap at EspHal::begin" figure that was
    // never verified against a real build — it
    // conflated available HEAP (what's left
    // after static allocation) with the DRAM
    // SEGMENT budget the linker actually
    // enforces (dram0_0_seg, shared statically
    // by the whole firmware image: Arduino
    // core, WiFi/BT buffers, every other
    // library's .bss/.data, not just what's
    // left over at runtime). The real
    // cross-compile against the field-tested
    // FireBeetle 2 ESP32-E target failed with
    // QUEUE_CAP=256 (Entry=184 B, ring=47304 B):
    // `ld: region 'dram0_0_seg' overflowed by
    // 3496 bytes`. QUEUE_CAP=128 (ring=23752 B)
    // cuts 23552 B — 3496 B needed plus >20 KB
    // margin for the rest of the sketch and any
    // future .bss growth elsewhere in the image.
    // The ring is a function-local static in
    // Log::log(), so it is charged to .bss
    // before setup() runs regardless of which
    // budget it's checked against. Any future
    // bump to either QUEUE_CAP or Entry::msg
    // fails this assert at compile time, not at
    // link time on real hardware. Pinned by
    // LogEntrySizeBudgetTest.
    static_assert(sizeof(LogRingBuffer<QUEUE_CAP + 1>) <= 25600,
                  "Log ring exceeds its 25 KB .bss budget (calibrated "
                  "against an observed dram0_0_seg link overflow, not a "
                  "heap-availability guess) — shrink Entry::msg or lower "
                  "QUEUE_CAP before booting on the field-tested device");

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
    // AL89-11: persistent count of dropped
    // lines. Bumped inside emit() on the
    // drop-oldest path. Test reset via
    // clearDroppedLines().
    mutable uint64_t droppedLines_ = 0;

    Log() {}
    Log(const Log &) = delete;
    Log &operator=(const Log &) = delete;

    void emit(const char *sev, const char *tag, const char *fmt,
              va_list ap) const;
};

} // namespace autolink
