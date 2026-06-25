// Per-cobsSeq ARQ state implementation.
#include "al/link/LinkArq.h"
#include "al/link/Link.h"
#include "al/hal/IHal.h"
#include "al/link/LinkDecision.h"
#include <cstring>

#ifdef ARDUINO
#    if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#        include <freertos/FreeRTOS.h>
#    endif
#endif

static constexpr const char *TAG = "AutoLink";
static constexpr uint8_t NO_BASE = 0xFF;

namespace autolink {
void LinkArq::clearAll() {
    memset(ackedPending_, 0, sizeof(ackedPending_));
    memset(retxCount_, 0, sizeof(retxCount_));
    memset(sentAtMs_, 0, sizeof(sentAtMs_));
    memset(baseSeq_, 0, sizeof(baseSeq_));
}

void LinkArq::onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs) {
    ackedPending_[seq] = true;
    retxCount_[seq] = 0;
    sentAtMs_[seq] = nowMs;
    baseSeq_[seq] = (baseSeq == NO_BASE) ? seq : baseSeq;
}

void LinkArq::onAcked(uint8_t seq) {
    ackedPending_[seq] = false;
    retxCount_[seq] = 0;
    baseSeq_[seq] = 0;
}

void LinkArq::onNaked(uint8_t missingCobsSeq, uint32_t nowMs) {
    if (!ackedPending_[missingCobsSeq])
        return;
    sentAtMs_[missingCobsSeq] = nowMs;
}

bool LinkArq::waitForAck(Link &l, uint8_t seq, uint32_t timeoutMs) {
    // Caller holds the lock. Drop it so the
    // link task can deliver the ACK; re-take
    // it on return so the caller keeps its
    // invariant of "lock held across the
    // whole frame".
    l.hwUnlock();
    uint32_t t0 = l.hwNowMs();
    while (ackedPending_[seq]) {
        if ((l.hwNowMs() - t0) >= timeoutMs) {
            l.hwLock();
            ackedPending_[seq] = false;
            retxCount_[seq] = 0;
            return false;
        }
#ifdef ARDUINO
        portYIELD();
#endif
    }
    l.hwLock();
    return true;
}

int LinkArq::pendingCount() const {
    int n = 0;
    for (int i = 0; i < 256; i++)
        if (ackedPending_[i])
            n++;
    return n;
}

LinkArq::Action LinkArq::decideSlot(uint8_t seq, uint32_t nowMs,
                                    uint32_t ackRtoMs, uint8_t maxRetx) const {
    uint32_t age = nowMs - sentAtMs_[seq];
    ArqAction a = decideArqSlot(age, retxCount_[seq], ackRtoMs, maxRetx);
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
    retxCount_[seq]++;
    sentAtMs_[seq] = nowMs;
    return seq;
}

void LinkArq::clearSlot(uint8_t seq) {
    ackedPending_[seq] = false;
    retxCount_[seq] = 0;
}

int LinkArq::popRetransmitSlot(uint32_t now, uint32_t ackRtoMs) {
    for (int s = 0; s < 256; s++) {
        if (!ackedPending_[s])
            continue;
        if (now - sentAtMs_[s] < ackRtoMs)
            continue;
        ackedPending_[s] = false;
        retxCount_[s]++;
        sentAtMs_[s] = now;
        return s;
    }
    return -1;
}

} // namespace autolink