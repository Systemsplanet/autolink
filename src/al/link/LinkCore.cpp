
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

#ifdef ARDUINO
#    include <esp_system.h>
#endif

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
    default:
        return "UNK";
    }
}

Link::Link(IHal &h, IArqCache &cache, bool isMasterNode,
           const AutoLinkConfig &config)
    : hw(h), arqCache_(cache), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.clampedCount()), rxIdx(0), frameRx(*this),
      lastRxMs(0), lastTxMs(0), txBytes(0), rxBytes(0), discCount(0),
      frameErrs(0) {
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate = config.minAcceptRate;
    sc.expectedSamples = -1;
    baudSweep.configure(sc);

    hw.setEvents(*this);
    Log::log().info(TAG, "Init as %s", isMaster ? "Ping" : "Pong");

    {
        const int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
        const int window = arqCache_.window();
        if (msgChunks > window) {
            Log::log().error(TAG,
                             "maxMsg=%u needs %d chunks > GBN window %d — "
                             "every ASYNC send will trip the window "
                             "admission guard; lower cfg.maxMsg, raise "
                             "MAX_CHUNK, or widen the window",
                             (unsigned)cfg.maxMsg, msgChunks, window);
        } else if (msgChunks * 2 > window) {
            Log::log().warning(
                TAG,
                "maxMsg=%u takes %d chunks — the window admission guard "
                "allows only one such message in flight at a time "
                "(window=%d)",
                (unsigned)cfg.maxMsg, msgChunks, window);
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

void Link::begin() {
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
        msgRx_.reset();
        frameRx.reset();
        baudSweep.resetAll();
        resetSeq_unlocked();
        sweep_.setPhase(SweepPhase::PHASE1);
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBaudSafe(spdI));
        Log::log().info(TAG, "SWP Pong P1 baud[%d]=%lu", spdI,
                        (unsigned long)cfg.allowedBaudSafe(spdI));

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

void Link::reset_unlocked(bool count, bool preservePreferredBaud) {
    if (count && state == State::OK) {
        discCount++;
        uint32_t dnow = hw.nowMs();
        // Kickoff-noise BREAKs (pre-first-lock) must not arm the
        // post-lock quiet gate; only a working link that dropped does.
        if (wasEverOk_) {
            recentDiscs_ = (lastDiscMs_ != 0 && dnow - lastDiscMs_ < 10000)
                ? recentDiscs_ + 1
                : 1;
            lastDiscMs_ = dnow;
        }
#ifdef ARDUINO

        Log::log().info(TAG, "resweep: disc=%lu freeHeap=%u",
                        (unsigned long)discCount,
                        (unsigned)esp_get_free_heap_size());
#endif
    }
    changeState_unlocked(State::SWP);

    spdI = 0;
    // BREAK-triggered resweep on the master consults the
    // proven baud (preferredBaud_) and tries a short-window
    // P3 re-lock at that baud before falling back to a full
    // P1 walk. The slave still goes to P1 slowest (the master
    // PING reaches the slave through MockHal/WireSim's baud-
    // matching filter only if they share a baud or the
    // simulation snaps the slave to the master's locked baud;
    // the slave's baud walk is what WireSim/recovery tests
    // exercise today, so keeping the slave's path stable).
    if (!preservePreferredBaud || !isMaster) {
        preferredBaud_ = NO_PREFERRED_BAUD;
        resweepPrefPending_ = false;
    }
    baudRetries_ = 0;
    pingSample = 0;
    rxIdx = 0;
    okCarryLen_ = 0;
    msgRx_.reset();
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    emptySweeps = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = hw.nowMs();
    txRejFirstMs_ = txRejLastMs_ = 0;
    arq_.clearAll();
    gbnAttempts_ = 0;
    gbnBackoffMs_ = 0;
    gbnLastRetxBase_ = 0xFF;
    resetSeq_unlocked();
    lastAckSeq_ = 0xFF;
    lastNakSeq_ = 0xFF;
    lastRxSeq_ = 0xFF;
    hw.clearAppBuf();
    // Master-on-BREAK with a recorded proven baud: short-window
    // P3 re-lock attempt; resweepPrefPending_ arms the P3 timeout
    // handler to fall back to enterPhase1 if the re-lock misses.
    if (preservePreferredBaud && isMaster && wasEverOk_ &&
        preferredBaud_ != NO_PREFERRED_BAUD &&
        preferredBaud_ < (uint8_t)cfg.clampToMaxBauds()) {
        spdI = preferredBaud_;
        sweep_.enterPhase3(*this, preferredBaud_);
        resweepPrefPending_ = true;
    } else {
        sweep_.enterPhase1(*this);
        resweepPrefPending_ = false;
    }
    arqCache_.clearAll();
    hw.discardTx();
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
