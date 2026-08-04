
#include "al/link/arq/LinkArq.h"
#include "al/link/IHalCtx.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/util/Log.h"
#include <cstring>

#ifdef ARDUINO
#    if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#        include <freertos/FreeRTOS.h>
#        include <freertos/semphr.h>
#    endif
#endif

static constexpr const char *TAG = "AutoLink";
static constexpr uint8_t NO_BASE = 0xFF;

#if defined(ARDUINO) && \
    (defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32))
// ack_sem_ is a void* in the header to dodge the
// freertos/semphr.h second-pass include trap. Cast
// back to the real handle at every FreeRTOS API
// call site. The cast is a no-op on the wire — the
// underlying storage is a QueueHandle_t either way.
static inline SemaphoreHandle_t ackSem(void *p) {
    return reinterpret_cast<SemaphoreHandle_t>(p);
}
#endif

namespace autolink {

void LinkArq::clearAll() {
    memset(bytesRecvd_, 0, sizeof(bytesRecvd_));
    memset(ackedPending_, 0, sizeof(ackedPending_));
    memset(retxCount_, 0, sizeof(retxCount_));
    memset(sentAtMs_, 0, sizeof(sentAtMs_));
    memset(baseSeq_, 0, sizeof(baseSeq_));
    gbnBase_ = 0;
    gbnActive_ = false;
    generation_++;
    // ARQ state reset: every pending slot, base, and retx counter
    // is wiped. The generation_ bump invalidates any caller that
    // unlocked around waitForAck — that caller now sees its
    // answer as "ack arrived, but a reset happened in between"
    // (the false return path) so it can re-evaluate against the
    // fresh session. Info level: this fires on every link reset
    // and is logged as the start of every new
    // session's ARQ bookkeeping.
    Log::log().info(TAG, "ARQ clearAll (gen=%lu)", (unsigned long)generation_);
#ifdef ARDUINO
    // Wake any waitForAck in flight on the prior session —
    // the false-return on the generation mismatch is the
    // caller's signal that the link was reset. Without
    // this give, a SYNC send wedged in waitForAck would
    // sit until its own timeout, with the reset event
    // already past.
    if (ack_sem_)
        xSemaphoreGive(ackSem(ack_sem_));
#endif
}

void LinkArq::onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs) {
    int bi = budgetIdx(seq);
    ackedPending_[bi] = true;
    retxCount_[bi] = 0;
    sentAtMs_[bi] = nowMs;
    baseSeq_[bi] = (baseSeq == NO_BASE) ? seq : baseSeq;
    // ARQ state transition: chunk now outstanding.
    // Per-frame trace; default-compiled-out. See the
    // field comment in LinkRx.cpp's onPayload. Pinned
    // by WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "ARQ onSent seq=%u base=%u gen=%lu (pending=%d)",
                       (unsigned)seq, (unsigned)baseSeq_[bi],
                       (unsigned long)generation_, pendingCount());
#endif
}

void LinkArq::rearmSlot(uint8_t seq, uint32_t nowMs) {
    // The chunk is still outstanding: pending / baseSeq / bytes
    // must survive, only the RTO clock and retx budget reset.
    int bi = budgetIdx(seq);
    retxCount_[bi] = 0;
    sentAtMs_[bi] = nowMs;
    Log::log().debug(TAG, "ARQ rearm seq=%u (RTO reset, pending survives)",
                     (unsigned)seq);
}

void LinkArq::onAcked(uint8_t seq, uint16_t bytesRecvd) {
    int bi = budgetIdx(seq);
    bytesRecvd_[bi] = bytesRecvd;
    ackedPending_[bi] = false;
    retxCount_[bi] = 0;
    // baseSeq_[bi] survives the ACK: bytesForMessage() walks it
    // afterwards.
    // ARQ state transition: chunk now acked. Per-frame
    // trace; default-compiled-out. See the field comment
    // in LinkRx.cpp's onPayload. Pinned by
    // WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "ARQ onAcked seq=%u bytes=%u (pending=%d)",
                       (unsigned)seq, (unsigned)bytesRecvd, pendingCount());
#endif
#ifdef ARDUINO
    // Wake the SYNC send waiting in waitForAck. One give
    // per onAcked is the right shape — the pending bit
    // just cleared and the waiter must re-evaluate; if
    // the waiter is for a different seq, the generation
    // check in waitForAck handles the race.
    if (ack_sem_)
        xSemaphoreGive(ackSem(ack_sem_));
#endif
}

uint16_t LinkArq::bytesFor(uint8_t seq) const {
    return bytesRecvd_[budgetIdx(seq)];
}

void LinkArq::onNaked(uint8_t missingCobsSeq, uint32_t nowMs) {
    if (!isPending(missingCobsSeq))
        return;
    sentAtMs_[budgetIdx(missingCobsSeq)] = nowMs;
    // NAK is the peer's explicit "this seq is missing" signal.
    // Reseating the RTO clock here lets sweepRetx fire on a fresh
    // window without immediately re-issuing the same RTO that just
    // got answered. Debug level: per-NAK in the wire path, but
    // never more than one per second per stuck base.
    Log::log().debug(TAG, "ARQ onNaked seq=%u (RTO reseated)",
                     (unsigned)missingCobsSeq);
}

void LinkArq::setPending(uint8_t seq, bool v) {
    Log::log().debug(TAG, "ARQ setPending seq=%u v=%d", (unsigned)seq,
                     v ? 1 : 0);
    ackedPending_[budgetIdx(seq)] = v;
}

bool LinkArq::isPending(uint8_t seq) const {
    // Side-effect-free on the hot path. Called from
    // waitForAck's spin, decideSlot sweeps, onAcked,
    // and onNaked — every iteration took the logger
    // path before the current shape, which (a) saturated the
    // log transport at 400+ chunks/s and (b) blocked
    // the spin when the log sink was wedged (a slow
    // web flush holding the log mutex), turning the
    // SYNC wait timeout check into a no-op. Pinned
    // by LinkArqIsPendingLogFreeTest.
    return ackedPending_[budgetIdx(seq)];
}

bool LinkArq::waitForAck(IHalCtx &ctx, uint8_t seq, uint32_t timeoutMs) {
    uint32_t genAtUnlock = generation_;
    // One NAK wake per ladder attempt: anything latched before this
    // wait belongs to a previous attempt and must not short-circuit
    // this one.
    nakWake_ = false;
    nakWakeSeq_ = 0xFF;
    ctx.hwUnlock();
#ifdef ARDUINO
    // Event-driven wait on a counting semaphore given by
    // onAcked / clearAll. The pre-the current shape shape was a
    // portYIELD() spin against isPending(), which (a)
    // burned the loop task at 100% CPU for the full
    // syncAckTimeoutMs window on every SYNC send and
    // (b) wedged the timeout check when the log sink
    // blocked the spin (the prior `ARQ isPending`
    // debug log went through a shared log mutex; a
    // slow web flush there meant the timeout branch
    // was unreachable). The sem-take path costs ~1
    // syscall per ACK arrival and zero CPU between
    // events.
    //
    // The waitForAck deadline MUST be a fixed
    // budget from t0 — not re-armed on every
    // foreign ACK. onAcked/clearAll give the
    // semaphore for *any* seq, and a successful
    // take continues the loop with a fresh full
    // timeoutMs slice under the naive
    // pdMS_TO_TICKS(timeoutMs) shape. With steady
    // ACK traffic for other seqs while the target
    // seq's ACK is lost (exactly the GBN pipeline
    // case where one chunk's ACK lands but the
    // rest are still in flight), the loop would
    // re-arm the full RTO forever and the SYNC
    // ladder's retx step would never fire.
    // Capture t0 once, take the *remaining* time
    // on each wake, and timeout when the
    // remainder hits zero. Pinned by
    // SyncWaitForAckDeadlinePinTest (foreign ACK
    // every 50 ms must still time out at ~timeoutMs).
    ensureAckSem_unlocked();
    uint32_t t0 = ctx.hwNowMs();
    uint32_t deadline = t0 + timeoutMs;
    while (isPending(seq)) {
        if (generation_ != genAtUnlock) {
            ctx.hwLock();
            return false;
        }
        if (nakWake_ && nakWakeSeq_ == seq) {
            ctx.hwLock();
            if (!isPending(seq)) {
                if (generation_ != genAtUnlock)
                    return false;
                return true;
            }
            int bi = budgetIdx(seq);
            ackedPending_[bi] = false;
            retxCount_[bi] = 0;
            Log::log().warning(TAG,
                               "ARQ waitForAck seq=%u woken by NAK -> retx now",
                               (unsigned)seq);
            return false;
        }
        uint32_t now = ctx.hwNowMs();
        if (now >= deadline) {
            ctx.hwLock();
            if (!isPending(seq)) {
                if (generation_ != genAtUnlock)
                    return false;
                return true;
            }
            int bi = budgetIdx(seq);
            ackedPending_[bi] = false;
            retxCount_[bi] = 0;
            Log::log().warning(TAG, "ARQ waitForAck seq=%u timeout after %u ms",
                               (unsigned)seq, (unsigned)timeoutMs);
            return false;
        }
        uint32_t remain = deadline - now;
        TickType_t slice = pdMS_TO_TICKS((TickType_t)remain);
        if (slice == 0)
            slice = 1;
        if (xSemaphoreTake(ackSem(ack_sem_), slice) == pdTRUE) {
            // Drained the sem — re-check pending
            // under the generation guard above.
            continue;
        }
        // Slice elapsed; loop and re-check the
        // deadline against now(). The FreeRTOS
        // tick is coarse (10 ms at 100 Hz) so a
        // take that returns pdFALSE because the
        // semaphore hasn't been given since the
        // last wake can shave a tick off the
        // remaining budget — that's fine, the
        // deadline is the source of truth.
    }
    ctx.hwLock();
    if (generation_ != genAtUnlock) {
        return false;
    }
    return true;
#else
    // Host tests run on a single thread: the wait and
    // the wake (onRx → onAck → onAcked) share a stack,
    // so a busy-spin is the cheapest test-side shape
    // and matches how the host suite drives the link
    // (pumpClock between pipe_data calls). The pre-
    // the current shape debug log is gone (isPending is side-
    // effect-free now), so the spin can't wedge the
    // test on a log-mutex deadlock.
    uint32_t t0 = ctx.hwNowMs();
    while (isPending(seq)) {
        if (nakWake_ && nakWakeSeq_ == seq) {
            ctx.hwLock();
            if (!isPending(seq)) {
                if (generation_ != genAtUnlock)
                    return false;
                return true;
            }
            int bi = budgetIdx(seq);
            ackedPending_[bi] = false;
            retxCount_[bi] = 0;
            Log::log().warning(TAG,
                               "ARQ waitForAck seq=%u woken by NAK -> retx now",
                               (unsigned)seq);
            return false;
        }
        if ((ctx.hwNowMs() - t0) >= timeoutMs) {
            ctx.hwLock();
            int bi = budgetIdx(seq);
            ackedPending_[bi] = false;
            retxCount_[bi] = 0;
            Log::log().warning(TAG, "ARQ waitForAck seq=%u timeout after %u ms",
                               (unsigned)seq, (unsigned)timeoutMs);
            return false;
        }
    }
    ctx.hwLock();
    if (generation_ != genAtUnlock) {
        return false;
    }
    return true;
#endif
}

#ifdef ARDUINO
// One-shot lazy init of the ack semaphore. Lives on
// the ARQ class instead of a free global so its
// lifetime matches LinkArq's. xSemaphoreCreateBinary
// starts empty — the producer (onAcked) gives before
// the first take, so the very first waitForAck is
// already past the post-init "take would block"
// hazard. Safe to call from a scheduler-running
// context (the first waitForAck is on the loop task,
// never the ctor).
void LinkArq::ensureAckSem_unlocked() {
    if (ack_sem_)
        return;
    SemaphoreHandle_t h = xSemaphoreCreateBinary();
    ack_sem_ = reinterpret_cast<void *>(h);
    if (!ack_sem_) {
        // Not fatal — waitForAck will fall back to
        // the spin in this pathological case, and the
        // SYNC timeout will still surface. The
        // accounting is the same; only the CPU cost
        // regresses. Logged as a one-shot error so
        // the operator can see the heap-pressure
        // signature in the field log.
        static bool noSemLogged_ = false;
        if (!noSemLogged_) {
            noSemLogged_ = true;
            Log::log().error(TAG,
                             "ack_sem_ create failed; "
                             "waitForAck will busy-spin");
        }
    }
}
#endif

int LinkArq::pendingCount() const {
    int n = 0;
    for (int i = 0; i < ARQ_CHUNK_BUDGET; i++)
        if (ackedPending_[i])
            n++;
    return n;
}

int LinkArq::retxCountTotal() const {
    int n = 0;
    for (int i = 0; i < ARQ_CHUNK_BUDGET; i++)
        n += retxCount_[i];
    return n;
}

uint8_t LinkArq::retxCountFor(uint8_t seq) const {
    return retxCount_[budgetIdx(seq)];
}

void LinkArq::noteNakWake(uint8_t seq) {
    nakWakeSeq_ = seq;
    nakWake_ = true;
#ifdef ARDUINO
    // Same wake channel onAcked/clearAll use; waitForAck re-checks
    // its exit conditions on every give.
    if (ack_sem_)
        xSemaphoreGive(ackSem(ack_sem_));
#endif
}

LinkArq::Action LinkArq::decideSlot(uint8_t seq, uint32_t nowMs,
                                    uint32_t ackRtoMs, uint8_t maxRetx) const {
    if (idxOf(seq) < 0)
        return Action::Hold;
    int bi = budgetIdx(seq);
    uint32_t age = nowMs - sentAtMs_[bi];
    ArqAction a = decideArqSlot(age, retxCount_[bi], ackRtoMs, maxRetx);
    switch (a) {
    case ArqAction::Hold:
        return Action::Hold;
    case ArqAction::Drop:
        return Action::Drop;
    case ArqAction::Retx:
        return Action::Retx;
    }
    return Action::Hold;
}

uint8_t LinkArq::applyRetx(uint8_t seq, uint32_t nowMs) {
    int bi = budgetIdx(seq);
    retxCount_[bi]++;
    sentAtMs_[bi] = nowMs;
    // Cold-path deep trace. Retx is called once per
    // sweep RTO verdict on a stuck base — at most
    // cfg.maxRetx times per base, never per-frame.
    // The on-the-spot operator lifts Log to DEBUG
    // to see the GBN ladder's retx budget burn
    // down. Pinned by SubsystemLoggingTest
    // LinkArq minDebug.
    Log::log().debug(TAG, "ARQ applyRetx seq=%u (retxCount++ base=%u)",
                     (unsigned)seq, (unsigned)retxCount_[bi]);
    return seq;
}

} // namespace autolink
