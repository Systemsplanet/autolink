// LinkTestAccessor implementation. Friend
// access into Link's private state. The
// shim is the only test surface that
// reaches Link's internals; production
// code does not include this TU.
#include "LinkTestAccessor.h"

#include "al/link/LinkArq.h"
#include "al/link/LinkReorder.h"
#include "al/util/UtilCrc.h"

#include <algorithm>
#include <cstring>

namespace autolink {

void LinkTestAccessor::markAckedPending(uint8_t s) {
    // Was Link::test_markAckedPending.
    l_.arq_.setPending(s, true);
}

bool LinkTestAccessor::sendMsgBegin(const uint8_t *b, int len) {
    // Was Link::test_sendMsgBegin.
    AutoLinkConfig &cfg = l_.cfg;
    IHal &hw = l_.hw;
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    if (len < 0 || (size_t)len > cfg.maxMsg)
        return false;
    if (len == 0)
        return true;
    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = { (uint8_t)(len),       (uint8_t)(len >> 8),
                             (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                             (uint8_t)(c),         (uint8_t)(c >> 8) };
    hw.lock();
    if (l_.state != State::OK) {
        hw.unlock();
        return false;
    }
    uint8_t seq = l_.txSeq;
    if (len + MSG_HDR <= MAX_CHUNK) {
        uint8_t merged[MAX_CHUNK];
        std::memcpy(merged, hdr, MSG_HDR);
        std::memcpy(merged + MSG_HDR, b, len);
        l_.sendCobsFrame_unlocked(merged, MSG_HDR + len);
        l_.txBytes += (uint64_t)len;
        l_.lastTxMs = hw.nowMs();
    } else {
        l_.sendCobsFrame_unlocked(hdr, MSG_HDR);
        l_.txBytes += 0;
        l_.lastTxMs = hw.nowMs();
        int offset = 0;
        while (offset < len) {
            int chunk = std::min(len - offset, MAX_CHUNK);
            seq = l_.txSeq;
            l_.sendCobsFrame_unlocked(b + offset, chunk);
            l_.txBytes += (uint64_t)chunk;
            l_.lastTxMs = hw.nowMs();
            offset += chunk;
        }
    }
    l_.arq_.onSent(seq, Link::NO_BASE, hw.nowMs());
    hw.unlock();
    return true;
}

bool LinkTestAccessor::sendMsgStillWaiting() {
    AutoLinkConfig &cfg = l_.cfg;
    IHal &hw = l_.hw;
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    hw.lock();
    bool any = l_.arq_.pendingCount() > 0;
    hw.unlock();
    return any;
}

int LinkTestAccessor::syncAckTimeoutMs() const {
    return l_.cfg.syncAckTimeoutMs;
}

bool LinkTestAccessor::reorderSlotInUse(uint8_t cobsSeq) const {
    return l_.reorder_.slotInUse(cobsSeq);
}

uint16_t LinkTestAccessor::reorderSlotLen(uint8_t cobsSeq) const {
    return l_.reorder_.slotLen(cobsSeq);
}

IArqCache *LinkTestAccessor::arqCache() const { return l_.arqCache_; }

} // namespace autolink