// Reorder buffer implementation.
#include "al/link/LinkReorder.h"
#include "al/link/Link.h"
#include "al/link/LinkFrameRx.h"
#include "al/util/Log.h"
#include <cstdlib>
#include <cstring>

static constexpr const char *TAG = "AutoLink";

namespace autolink {
LinkReorder::~LinkReorder() { clearAll(); }

void LinkReorder::clearAll() {
    for (int i = 0; i < 256; i++) {
        if (slots_[i].buf)
            free(slots_[i].buf);
        slots_[i].buf = nullptr;
        slots_[i].in_use = false;
        slots_[i].len = 0;
    }
}

int LinkReorder::dropExpired(uint32_t nowMs, int holdMs) {
    int dropped = 0;
    for (int i = 0; i < 256; i++) {
        if (!slots_[i].in_use)
            continue;
        uint32_t age = nowMs - slots_[i].heldAtMs;
        if (age < (uint32_t)holdMs)
            continue;
        free(slots_[i].buf);
        slots_[i].buf = nullptr;
        slots_[i].in_use = false;
        slots_[i].len = 0;
        dropped++;
        Log::log().warning(TAG,
                           "reorder seq=%u expired "
                           "(%ums)",
                           (unsigned)i, (unsigned)age);
    }
    return dropped;
}

int LinkReorder::flushContiguous(Link &l, uint32_t nowMs) {
    (void)nowMs;
    int delivered = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t exp = l.reorderExpectedSeq();
        if (!slots_[exp].in_use)
            break;
        Slot &s = slots_[exp];
        int acc = l.reorderPushAppBuf(s.buf, s.len);
        if (acc < s.len) {
            Log::log().info(TAG,
                            "reorder flush seq=%u "
                            "buf full (want %d got %d)",
                            (unsigned)exp, (int)s.len, acc);
            break;
        }
        l.reorderSendAck(exp);
        l.reorderAdvanceRxSeq(exp);
        l.reorderCountBytes(s.len);
        free(s.buf);
        s.buf = nullptr;
        s.in_use = false;
        s.len = 0;
        delivered++;
        progress = true;
    }
    return delivered;
}

bool LinkReorder::hold(uint8_t seq, const uint8_t *b, int n, uint32_t nowMs) {
    if (n <= 0)
        return false;
    uint8_t *slotBuf = (uint8_t *)malloc(n);
    if (!slotBuf)
        return false;
    memcpy(slotBuf, b, n);
    if (slots_[seq].in_use) {
        free(slots_[seq].buf);
        slots_[seq].buf = slotBuf;
        slots_[seq].len = (uint16_t)n;
        slots_[seq].heldAtMs = nowMs;
        slots_[seq].in_use = true;
        return false;
    }
    slots_[seq].buf = slotBuf;
    slots_[seq].len = (uint16_t)n;
    slots_[seq].heldAtMs = nowMs;
    slots_[seq].in_use = true;
    return true;
}

} // namespace autolink