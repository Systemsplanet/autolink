
#pragma once
#include <stdint.h>
#include <climits>
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#endif
#include "al/AutoLinkConfig.h"
#include "al/util/log/Log.h"

namespace autolink {

class ILinkEvents {
public:
    virtual ~ILinkEvents() = default;
    virtual void onRx(const uint8_t *data, int len) = 0;
    virtual void onBreak() = 0;
    virtual void onTimer() = 0;
    // One shot per window: a sustained BREAK pattern
    // (suppressed_count >= BREAK_STORM_THRESHOLD) is a
    // baud-mismatch signature. The link layer must
    // abandon any preserved-baud fast path and force a
    // P1 walk; the HAL is timing-only and cannot act on
    // its own.
    virtual void onBreakStorm() { /* optional */ }
    // Per-tx wall-time cost. The HAL measures dt inside
    // tx() and forwards it to the link layer so the
    // link can keep a running txBlockedMs_ aggregate
    // for the periodic stats line. Earlier shape
    // measured dt but never propagated it, so the
    // operator's stats line printed txBlockedMs=0
    // regardless of the wire's contention. Default
    // no-op for ILinkEvents implementations that
    // don't care (test stubs). Pinned by
    // StatsTxBlockedMsTest.
    virtual void onTxBlockedNote(uint32_t ms) { (void)ms; }
};

class IHal {
public:
    virtual ~IHal() = default;

    void setEvents(ILinkEvents &e) {
#ifdef AUTOLINK_HOST_TEST
        assert(events_ == nullptr && "setEvents called twice on the same HAL");
#endif
        if (events_ != nullptr) {
            Log::log().error("IHal",
                             "setEvents called twice — "
                             "rebinding listener");
        }
        events_ = &e;
    }
    ILinkEvents *events() const { return events_; }

    virtual bool isHealthy() const { return true; }

    virtual void begin() = 0;
    // Config-aware begin, reachable through an IHal& so Link::begin() can
    // size the HAL itself. The default forwards to begin() for HALs that
    // do not size from config; EspHal and MockHal override to apply the
    // buffer floors. Pinned by LinkBeginInstallsHalTest.
    virtual void begin(const AutoLinkConfig &cfg) {
        (void)cfg;
        begin();
    }
    virtual void setSpd(uint32_t s) = 0;
    virtual void sendBreak() = 0;

    virtual void setMode(AutoLinkConfig::Mode) {}
    // AutoLink::begin() compares the facade's cfg.mode
    // against the HAL's stored mode and logs an error
    // if they disagree. The HAL must therefore be
    // able to report its own mode (EspHal's copy
    // could in principle diverge from the AutoLink's
    // if a future refactor forwards setMode to one
    // but not the other; the begin-time check catches
    // it before the link starts dropping frames).
    virtual AutoLinkConfig::Mode getMode() const {
        return AutoLinkConfig::Mode::SYNC;
    }

    virtual int tx(const uint8_t *b, int n) = 0;
    // Free bytes in the UART TX ring. Used by the
    // GBN retx path to bound a burst against the
    // hardware queue (otherwise an 8-frame retx
    // blocks the link lock for ~31 ms at 512000
    // baud). Returns a very-large default for HALs
    // that don't model the ring; the real value is
    // queried from uart_get_tx_buffer_free_size on
    // ESP32. Pinned by UartTxBufferRetxBurstTest.
    virtual int txAvail() const { return INT32_MAX; }
    // I1: the installed TX ring size
    // (after the heap-cap in
    // uartTxBufferFloorCapped). Link::begin
    // gates on this rather than
    // re-deriving the floor from cfg —
    // the HAL may have installed a
    // smaller ring (heap-clamped)
    // and the gate has to see what
    // the wire actually has. Default
    // 0 = "not tracked", which the
    // gate treats as "skip the check"
    // (back-compat for HALs that
    // don't model a ring). Pinned by
    // BeginRejectsHeapClampedRingTest.
    virtual size_t txRingSize() const { return 0; }
    // The installed receiver stream-buffer size (or
    // 0 for HALs that don't model one). The
    // AL89-4 receiver-capacity clamp uses it to size
    // the GBN window against the buffer the
    // receiver can actually accept, so a window
    // that overruns a 4108 B stream buffer on a
    // default config can never enter the pipeline
    // in the first place. Default 0 = "not
    // tracked", which the gate treats as
    // "skip the check" (back-compat for HALs that
    // don't model a stream buffer). Pinned by
    // ArqWindowClampedToReceiverTest.
    virtual size_t rxRingSize() const { return 0; }
    // RX overflow / framing error counters.
    // Incremented from the UART event task on
    // UART_FIFO_OVF / UART_BUFFER_FULL /
    // UART_PARITY_ERR / UART_FRAME_ERR. Exposed
    // so the link layer can surface a total RX
    // blackout in the periodic stats line — a
    // blackout is silent otherwise (no log line
    // at all on the worst path). Pinned by
    // EspHalUartOverflowCountersTest.
    virtual uint64_t rxOverflowCount() const { return 0; }
    virtual uint64_t rxFrameErrCount() const { return 0; }
    virtual void flushTx() = 0;
    // Best-effort drain of bytes already queued for TX, so a link
    // reset doesn't spill pre-BREAK bytes into the next baud.
    virtual void discardTx() {}
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
    virtual void delayMs(int ms) = 0;
    // Sub-tick pacing (the ASYNC inter-chunk gap). Defaults to the
    // ms-level primitive; ESP32 overrides with ets_delay_us.
    virtual void delayUs(uint32_t us) { delayMs((int)((us + 999) / 1000)); }
    virtual uint32_t nowMs() = 0;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    // Bounded-wait variant for UART-event-task callers (onBreak,
    // onBreakStorm). sendMsg can hold the link lock across a
    // blocking hw.tx(); an indefinite lock() from the event task
    // parks the RX drainer behind it. Default forwards to lock()
    // and always succeeds — HALs that don't model contention (host
    // tests) keep today's unconditional behavior; EspHal overrides
    // with a real bounded xSemaphoreTake. Pinned by
    // EventTaskBoundedLockTest.
    virtual bool tryLock(int timeoutMs) {
        (void)timeoutMs;
        lock();
        return true;
    }

    virtual int pushAppBuf(const uint8_t *b, int n) = 0;
    virtual int popAppBuf(uint8_t *b, int max_len) = 0;
    virtual int peekAppBuf() const = 0;
    virtual int appBufAvailable() const = 0;
    // Free space in the app-side receive buffer. The
    // default returns "infinite" (the old "always room"
    // contract that the buggy-original onPayload() relied on);
    // ESP32 / MockHal override to the real
    // (capacity - available) for the all-or-nothing
    // admission check. Pinned by
    // AppBufFullAdmitNothingTest.
    virtual int appBufFree() const { return INT32_MAX; }
    virtual int peekAt(uint8_t *out, int n, int offset) const = 0;
    virtual void clearAppBuf() = 0;

    virtual void flushRxHw() {}

    // Link state, mirrored to the HAL so a raw event-task context
    // (no access to Link's locked state) can gate a hardware side
    // effect on it. Default no-op: only EspHal's UART-BREAK path
    // currently needs this. Called from changeState_unlocked on
    // every state transition. Pinned by EspHalBreakFlushGuardTest.
    virtual void setOkState(bool) {}

private:
    ILinkEvents *events_ = nullptr;
};

} // namespace autolink
