
#include "al/link/arq/LinkArq.h"
#include "al/link/IHalCtx.h"
#include "al/link/sweep/LinkDecision.h"
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
    memset(bytesRecvd_, 0, sizeof(bytesRecvd_));
    memset(ackedPending_, 0, sizeof(ackedPending_));
    memset(retxCount_, 0, sizeof(retxCount_));
    memset(sentAtMs_, 0, sizeof(sentAtMs_));
    memset(baseSeq_, 0, sizeof(baseSeq_));
    gbnBase_ = 0;
    gbnActive_ = false;
    generation_++;
}

void LinkArq::onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs) {
    // budgetIdx (not idxOf): must stay resolvable after this seq
    // is ACKed and gbnBase_ has moved past it — sentAtMs_ needs
    // this exactly as much as the other four fields do, so all
    // five share one index. Safe to overwrite a stale occupant
    // here — at most W (window) chunks are ever in flight and
    // budgetIdx wraps every 2*W sends, so a slot's previous
    // occupant is always ACKed before onSent can reuse its
    // budgetIdx.
    int bi = budgetIdx(seq);
    ackedPending_[bi] = true;
    retxCount_[bi] = 0;
    sentAtMs_[bi] = nowMs;
    baseSeq_[bi] = (baseSeq == NO_BASE) ? seq : baseSeq;
}

void LinkArq::onAcked(uint8_t seq, uint16_t bytesRecvd) {
    int bi = budgetIdx(seq);
    bytesRecvd_[bi] = bytesRecvd;
    ackedPending_[bi] = false;
    retxCount_[bi] = 0;
    // baseSeq_[bi] is deliberately left alone — bytesForMessage()
    // walks it AFTER the ACK, so clearing it here would erase the
    // very association the caller is about to ask for.
}

uint16_t LinkArq::bytesFor(uint8_t seq) const {
    return bytesRecvd_[budgetIdx(seq)];
}

void LinkArq::onNaked(uint8_t missingCobsSeq, uint32_t nowMs) {
    if (!isPending(missingCobsSeq))
        return;
    sentAtMs_[budgetIdx(missingCobsSeq)] = nowMs;
}

void LinkArq::setPending(uint8_t seq, bool v) {
    ackedPending_[budgetIdx(seq)] = v;
}

bool LinkArq::isPending(uint8_t seq) const {
    return ackedPending_[budgetIdx(seq)];
}

bool LinkArq::waitForAck(IHalCtx &ctx, uint8_t seq, uint32_t timeoutMs) {
    uint32_t genAtUnlock = generation_;
    ctx.hwUnlock();
    uint32_t t0 = ctx.hwNowMs();
    while (isPending(seq)) {
        if ((ctx.hwNowMs() - t0) >= timeoutMs) {
            ctx.hwLock();
            int bi = budgetIdx(seq);
            ackedPending_[bi] = false;
            retxCount_[bi] = 0;
            return false;
        }
#ifdef ARDUINO
        portYIELD();
#endif
    }
    ctx.hwLock();
    if (generation_ != genAtUnlock) {
        return false;
    }
    return true;
}

int LinkArq::pendingCount() const {
    int n = 0;
    for (int i = 0; i < ARQ_CHUNK_BUDGET; i++)
        if (ackedPending_[i])
            n++;
    return n;
}

LinkArq::Action LinkArq::decideSlot(uint8_t seq, uint32_t nowMs,
                                    uint32_t ackRtoMs, uint8_t maxRetx) const {
    // idxOf's only remaining job: reject a decision for a seq
    // that isn't currently within the tracked pending window at
    // all. The actual per-slot data lives at budgetIdx(seq),
    // which stays correct across a gbnBase_ advance — idxOf's
    // relative offset would not (see the class comment).
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
    return seq;
}

} // namespace autolink
