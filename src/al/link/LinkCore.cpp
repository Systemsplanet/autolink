// LinkCore -- ctor, begin, kickoff, changeState, reset_unlocked,
// getters (getState/getErrCount/getCurrentSpdIndex/getCurrentBaud/
// getDiag/getStats), resetStats/resetErrors/resetDiag, resetSeq.
// StateToStr lives here too because every TU that logs the state
// name needs it.
//
// LockOk is in LinkSweep.cpp because it interacts with the SWP
// helpers; the SWP -> OK transition is one half of the sweep
// phase machine. LockOk_unlocked calls okTickMs() which is also
// in LinkSweep.cpp; both TUs #include Link.h so the linker
// resolves the call across TUs.
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

const char *StateToStr(State s) {
    switch (s) {
    case State::OK:
        return "OK";
    case State::SWP:
        return "SWP";
    case State::LCK:
        return "LCK";
    default:
        return "UNK";
    }
}

Link::Link(IHal &h, IArqCache &cache, bool isMasterNode,
           const AutoLinkConfig &config)
    : hw(h), arqCache_(cache), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.clampedCount()), rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0),
      txBytes(0), rxBytes(0), discCount(0), frameErrs(0) {
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate = config.minAcceptRate;
    sc.expectedSamples = -1;
    baudSweep.configure(sc);
    // One-shot listener wire-up. The HAL holds
    // an ILinkEvents*; we satisfy that with
    // *this. The HAL never sees a Link* — the
    // cycle through IHal::link is closed.
    hw.setEvents(*this);
    Log::log().info(TAG, "Init as %s", isMaster ? "Ping" : "Pong");
    if (cfg.maxMsg > cfg.streamBufferSize)
        Log::log().error(TAG,
                         "maxMsg > streamBufSize: "
                         "large msgs will be dropped");
    // Budget-vs-msg sanity. The runtime guard
    // in sendMsg() rejects a send when
    // inflight + chunks > COBS_SEQ_SPACE. That
    // guard is only meaningful if at least one
    // full message of chunks fits the seq space
    // alone (otherwise the very first send
    // trips the guard with inflight=0). Compute
    // the chunk count for the configured
    // maxMsg; if it exceeds COBS_SEQ_SPACE, the
    // user-supplied maxMsg is too large for the
    // wire protocol — every send will be
    // dropped at runtime. Log loud; the
    // static_asserts in AutoLinkConfig.h only
    // cover the compile-time default of 1024.
    {
        const int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
        if (msgChunks > COBS_SEQ_SPACE) {
            Log::log().error(TAG,
                             "maxMsg=%u needs %d chunks > seq space %d — "
                             "every send will trip the seq-space guard; "
                             "lower cfg.maxMsg or raise MAX_CHUNK",
                             (unsigned)cfg.maxMsg, msgChunks, COBS_SEQ_SPACE);
        } else if ((size_t)msgChunks * 2 > (size_t)arqCache_.size() + 32) {
            // Soft warning: at this size, a
            // single message consumes most of the
            // seq space, leaving little headroom
            // for in-flight concurrency. Two
            // such messages back-to-back can
            // alias. The 32-byte slack is
            // hand-tuned for the typical
            // window=32 / pool=64 cache.
            Log::log().warning(
                TAG,
                "maxMsg=%u takes %d chunks — "
                "seq-space guard will reject at ~%d inflight messages",
                (unsigned)cfg.maxMsg, msgChunks,
                COBS_SEQ_SPACE / (msgChunks ? msgChunks : 1));
        }
    }
}

Link::~Link() = default;

void Link::resetSeq_unlocked() {
    txSeq = 0;
    rxSeqSet = false;
    rxSeq = 0;
}

uint8_t Link::reorderExpectedSeq() const {
    return (uint8_t)((rxSeq == (uint8_t)COBS_SEQ_MAX) ? 0 : rxSeq + 1);
}

void Link::reorderAdvanceRxSeq(uint8_t seq) {
    rxSeq = seq;
    rxSeqSet = true;
}

void Link::begin() {
    // Pure init: dwell computation, internal state.
    // Wire-side effects (sendBreak, sweep entry, baud
    // arm) happen via kickoff(). When linkPaused_ is
    // false (the default — host tests, Pong, etc.),
    // kickoff fires here so the legacy "begin() = go"
    // contract holds; when the caller has set
    // linkPaused_=true before begin() (Ping's startup-
    // order case), the kickoff is deferred to the
    // explicit kickoff() call that fires when the user
    // pushes Start.
    hw.lock();
    sweep_.computeDwells(*this);
    hw.unlock();
    kickedOff_ = false;
    if (linkPaused_) {
        Log::log().info(TAG,
                        "begin: link initialised; kickoff deferred "
                        "(linkPaused=true)");
        return;
    }
    kickoff();
}

void Link::kickoff() {
    if (kickedOff_) {
        Log::log().debug(TAG, "kickoff: already running; no-op");
        return;
    }
    if (linkPaused_) {
        Log::log().warning(TAG,
                           "kickoff: linkPaused_=true; refusing to fire "
                           "wire-side start. Call setLinkPaused(false) "
                           "first.");
        return;
    }
    kickedOff_ = true;
    if (isMaster) {
        hw.lock();
        reset_unlocked(false);
        sweep_.enterPhase1(*this);
        hw.unlock();
        hw.sendBreak();
        Log::log().info(TAG, "kickoff: master sent break; entering P1");
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = cfg.clampToMaxBauds() - 1;
        pingSample = 0;
        rxIdx = 0;
        rxMsgLen = -1;
        frameRx.reset();
        baudSweep.resetAll();
        resetSeq_unlocked();
        sweep_.setPhase(SweepPhase::PHASE1);
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBaudSafe(spdI));
        Log::log().info(TAG, "SWP Pong P1 baud[%d]=%lu", spdI,
                        (unsigned long)cfg.allowedBaudSafe(spdI));
        // Slave must outlast one full master P2 dwell so the
        // slave's P1 listen window covers the master's PING
        // without being raced by the master's P2 timer.
        hw.startTimer(sweep_.dwells().phase2[0] + 200);
        Log::log().info(TAG, "kickoff: slave armed P1 listener");
    }
}

void Link::changeState_unlocked(State newState) {
    if (state != newState) {
        Log::log().debug(TAG, "%s -> %s", StateToStr(state),
                         StateToStr(newState));
        state = newState;
    }
}

void Link::reset_unlocked(bool count) {
    if (count && state == State::OK)
        discCount++;
    changeState_unlocked(State::SWP);
    // Always restart P1 at slowest baud.
    // Honoring preferredBaud_ risks
    // re-locking on the baud that failed.
    spdI = 0;
    preferredBaud_ = NO_PREFERRED_BAUD;
    baudRetries_ = 0;
    pingSample = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = lckRetries = 0;
    emptySweeps = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = hw.nowMs();
    arq_.clearAll();
    hasPendingRetx_ = false;
    pendingRetxBase_ = NO_BASE;
    reorder_.clearAll();
    resetSeq_unlocked();
    lastAckSeq_ = 0xFF;
    lastNakSeq_ = 0xFF;
    lastRxSeq_ = 0xFF;
    memset(bytesRecvd_, 0, sizeof(bytesRecvd_));
    hw.clearAppBuf();
    sweep_.enterPhase1(*this);
    arqCache_.clearAll();
}

void Link::getStats(Stats &s) const {
    hw.lock();
    s.tx = txBytes;
    s.rx = rxBytes;
    s.discCount = discCount;
    s.frameErrs = frameErrs;
    hw.unlock();
}
void Link::resetStats() {
    hw.lock();
    txBytes = rxBytes = 0;
    hw.unlock();
}
void Link::resetErrors() {
    hw.lock();
    discCount = frameErrs = 0;
    errWindowCount_ = 0;
    errWindowStartMs_ = hw.nowMs();
    hw.unlock();
}
void Link::resetDiag() {
    hw.lock();
    gaps = stale = lostMsgs = 0;
    hw.unlock();
}

State Link::getState() const {
    hw.lock();
    State s = state;
    hw.unlock();
    return s;
}
int Link::getErrCount() const {
    hw.lock();
    int e = errs;
    hw.unlock();
    return e;
}
int Link::getCurrentSpdIndex() const {
    hw.lock();
    int i = spdI;
    hw.unlock();
    return i;
}
uint32_t Link::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < cfg.clampedCount())
        ? cfg.allowedBaudSafe(spdI)
        : 0;
    hw.unlock();
    return b;
}
void Link::getDiag(Diag &d) const {
    hw.lock();
    d.txSeq = txSeq;
    d.rxSeqSet = rxSeqSet;
    d.rxSeq = rxSeq;
    d.gaps = gaps;
    d.stale = stale;
    d.lostMsgs = lostMsgs;
    d.baudRetries = (uint64_t)baudRetries_;
    d.preferredBaud = preferredBaud_;
    hw.unlock();
}

} // namespace autolink
