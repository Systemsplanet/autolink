
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/arq/IArqCache.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"
#include "al/link/LinkMsgCodec.h"
#include <stdlib.h>

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

int Link::findMsgHeaderResync_unlocked(int max_scan) {
    int avail = hw.appBufAvailable();
    if (avail < MSG_HDR)
        return -1;
    int scan = (max_scan > 0 && max_scan < avail) ? max_scan : avail;
    int snapLen = scan + MSG_HDR;
    if (snapLen > avail)
        snapLen = avail;
    uint8_t *snap = (uint8_t *)malloc(snapLen);
    if (!snap)
        return -1;
    int got = hw.popAppBuf(snap, snapLen);
    int d = msgResyncScan(snap, got, cfg.maxMsg);
    if (d >= 0) {
        hw.pushAppBuf(snap + d, got - d);
        free(snap);
        return d;
    }
    free(snap);
    hw.clearAppBuf();
    return -1;
}

void Link::onRx(const uint8_t *data, int len) {
    hw.lock();
    int i = 0;
    lastRxMs = hw.nowMs();
    bool needBreak = false;
    while (i < len) {
        State cur = state;
        if (cur == State::OK) {
            if (okCarryLen_ > 0) {
                // Complete (or disqualify) the CTRL candidate held
                // from the previous chunk's tail.
                while (okCarryLen_ < CTRL_FRAME_SIZE && i < len)
                    okCarry_[okCarryLen_++] = data[i++];
                if (okCarryLen_ < CTRL_FRAME_SIZE)
                    break;
                if (okCarry_[1] == 0x55 &&
                    UtilCrc::crc8(okCarry_, CTRL_FRAME_SIZE - 1) ==
                        okCarry_[CTRL_FRAME_CRC_IDX]) {
                    for (int k = 0; k < CTRL_FRAME_SIZE; k++)
                        rxBuf[k] = okCarry_[k];
                    rxIdx = 0;
                    okCarryLen_ = 0;
                    if (processCtrlFrame_unlocked(cur))
                        needBreak = true;
                    continue;
                }
                // Not a CTRL frame: all held bytes (including the
                // 0xAA — it is a payload byte inside a COBS frame)
                // go to the framer, matching the in-chunk CRC-fail
                // path. A nested CTRL start inside these bytes is
                // lost (retransmit recovers).
                okCarryLen_ = 0;
                frameRx.feed(okCarry_, CTRL_FRAME_SIZE);
                if (state != State::OK)
                    needBreak = true;
                continue;
            }
            int start = i;
            while (i < len) {
                uint8_t b = data[i];
                if (b == 0xAA && (len - i) >= CTRL_FRAME_SIZE &&
                    data[i + 1] == 0x55) {
                    // Pre-validate CRC8 on the 5-byte CTRL candidate.
                    // The OK-state payload can be any byte sequence
                    // (COBS guarantees no 0x00; every other value is
                    // free), so the `0xAA 0x55` sentinel collides
                    // with random payload data at ~1/65536 per byte.
                    // Each collision used to consume 5 bytes as if
                    // they were a CTRL frame; the CRC8 fail in
                    // processCtrlFrame_unlocked then tripped
                    // err_unlocked() -> frameErrs++ -> errThreshold
                    // drop. That made the wire-framing collision
                    // safe only for low-entropy sequential fill;
                    // random content (the 07/07 bench's RANDOM Ping
                    // fill) hit the threshold and dropped the link.
                    // Now verify CRC up front, and on a fail skip
                    // only the 0xAA so the bytes still reach
                    // frameRx.feed as COBS payload (where they
                    // decode normally — no 0x00 inside).
                    uint8_t cand[CTRL_FRAME_SIZE] = { 0xAA, 0x55, data[i + 2],
                                                      data[i + 3],
                                                      data[i + 4] };
                    if (UtilCrc::crc8(cand, CTRL_FRAME_SIZE - 1) ==
                        cand[CTRL_FRAME_CRC_IDX]) {
                        if (i > start) {
                            int c = frameRx.feed(data + start, i - start);
                            if (state != State::OK) {
                                i = start + c;
                                break;
                            }
                        }
                        for (int k = 0; k < CTRL_FRAME_SIZE; k++)
                            rxBuf[k] = data[i + k];
                        i += CTRL_FRAME_SIZE;
                        rxIdx = 0;
                        if (processCtrlFrame_unlocked(cur))
                            needBreak = true;
                        start = i;
                        continue;
                    }
                    // Not a CTRL frame: drop the 0xAA and let the
                    // framer see these bytes as part of a COBS
                    // payload (the byte we just consumed is 0xAA,
                    // which is a valid COBS run-code byte).
                    i++;
                    continue;
                }
                if (b == 0xAA && (len - i) < CTRL_FRAME_SIZE &&
                    ((len - i) < 2 || data[i + 1] == 0x55)) {
                    // Possible CTRL frame split across delivery
                    // chunks (UART reads land on arbitrary byte
                    // boundaries): flush scanned payload, hold the
                    // tail for the next onRx. Feeding it to the
                    // COBS framer instead corrupts the stream —
                    // CTRL bytes may contain 0x00.
                    if (i > start) {
                        int c = frameRx.feed(data + start, i - start);
                        if (state != State::OK) {
                            i = start + c;
                            break;
                        }
                    }
                    while (i < len)
                        okCarry_[okCarryLen_++] = data[i++];
                    start = i;
                    break;
                }
                i++;
            }
            if (i > start && state == State::OK) {
                int c = frameRx.feed(data + start, i - start);
                i = start + c;
            }
            if (state != State::OK)
                needBreak = true;
        } else {
            uint8_t b = data[i++];
            if (rxIdx == 0 && b != 0xAA)
                continue;
            if (rxIdx == 1 && b != 0x55) {
                rxIdx = 0;
                continue;
            }
            rxBuf[rxIdx++] = b;
            if (rxIdx == CTRL_FRAME_SIZE) {
                rxIdx = 0;
                if (processCtrlFrame_unlocked(cur)) {
                    needBreak = true;
                    break;
                }
            }
        }
    }
    hw.unlock();
    if (needBreak)
        hw.sendBreak();
}

bool Link::processCtrlFrame_unlocked(State cur) {
    if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) !=
        rxBuf[CTRL_FRAME_CRC_IDX]) {
        return err_unlocked();
    }
    uint8_t cs = rxBuf[CTRL_FRAME_SEQ_IDX];
    uint8_t pl = rxBuf[CTRL_FRAME_PAYLOAD_IDX];
    if (cur == State::OK) {
        if (pl == PING_CMD)
            sendPongAck_unlocked();
        return false;
    }
    return ctrlFrameReady_unlocked(cs, pl, cur);
}

bool Link::ctrlFrameReady_unlocked(uint8_t cs, uint8_t pl, State cur) {
    if (cur == State::SWP)
        return handleSwp_unlocked(cs, pl);
    return false;
}

bool Link::onPayload(uint8_t cobsSeq, const uint8_t *b, int n) {
    if (state != State::OK)
        return true;

    lastRxSeq_ = cobsSeq;

    int diff = 0;
    GapClass cls = classifyGap(cobsSeq, rxSeq, rxSeqSet, &diff);
    // Duplicate of a delivered frame (its ACK was lost): re-ACK so
    // the sender frees the slot. Wrap-safe only because the sender's
    // stalled-window gate keeps every pending seq within 96 of txSeq,
    // so a live duplicate can't collide with a later-wrap frame.
    if (cls == GapClass::Stale) {
        stale++;
        sendAckFrame_unlocked(cobsSeq, (uint16_t)(n > 0 ? n : 0));
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
            gaps++;
            lostMsgs += (uint64_t)diff;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }
#ifdef AUTOLINK_HOST_TEST
        if (testForwardResync_) {
            gaps++;
            lostMsgs += (uint64_t)diff;
            rxSeq = cobsSeq;
            rxSeqSet = true;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }
#endif
        // GBN receiver: in-order-only accept. A frame that
        // arrives ahead of the expected seq is dropped (not
        // held/reordered) — the missing seq's own NAK, driven
        // to the sender's oldest-unacked, is the only recovery
        // path. Every out-of-order arrival re-NAKs the expected
        // seq (fast-retransmit-style signal).
        uint8_t exp = reorderExpectedSeq();
        gaps++;
        lostMsgs++;
        Log::log().info(TAG, "GAP seq=%u exp=%u diff=%d (dropped)",
                        (unsigned)cobsSeq, (unsigned)exp, diff);
        sendNakFrame_unlocked(exp);
        return false;
    }
    rxSeq = cobsSeq;
    rxSeqSet = true;
    if (n == 0) {
        if (errs > 0)
            errs = 0;
        return false;
    }
    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    if (decideAppBuf(acc, n) == AppBufAction::HoldAck) {
        Log::log().warning(TAG,
                           "seq=%u app buf full "
                           "(want %d got %d) — sending NAK",
                           (unsigned)cobsSeq, n, acc);
        sendNakFrame_unlocked(cobsSeq);
        return false;
    }
    // Cumulative ACK: this frame's seq is now the highest
    // contiguous seq delivered.
    sendAckFrame_unlocked(cobsSeq, (uint16_t)n);
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(ackedCobsSeq))
        return false;
    lastAckSeq_ = ackedCobsSeq;
    recentDiscs_ = 0;
    rxBytes += RX_ACK_WIRE_BYTES;

    if (cfg.mode == AutoLinkConfig::Mode::SYNC || !arq_.gbnActive()) {
        arq_.onAcked(ackedCobsSeq, bytesRecvd);
        arqCache_.freeBySeq(ackedCobsSeq);
        return false;
    }

    // GBN cumulative ACK: ackedCobsSeq is the new highest
    // contiguous seq the receiver holds, so free every slot
    // from the current base through it. Interior slots (whose
    // own ACK never arrived — the reason base was stuck) are
    // backfilled from the sender's own cache record of what it
    // queued, since a cumulative ACK proves they were delivered
    // byte-for-byte as sent.
    uint8_t s = arq_.gbnBase();
    for (;;) {
        uint16_t bytes = bytesRecvd;
        if (s != ackedCobsSeq) {
            const uint8_t *buf = nullptr;
            int len = 0;
            bytes = arqCache_.peekForRetx(s, &buf, &len) ? (uint16_t)len : 0;
        }
        arq_.onAcked(s, bytes);
        arqCache_.freeBySeq(s);
        if (s == ackedCobsSeq)
            break;
        s = (s == COBS_SEQ_MAX) ? 0 : (uint8_t)(s + 1);
    }
    arq_.setGbnBase(
        (ackedCobsSeq == COBS_SEQ_MAX) ? 0 : (uint8_t)(ackedCobsSeq + 1));
    gbnAttempts_ = 0;
    gbnBackoffMs_ = 0;
    gbnLastRetxBase_ = 0xFF;
    arq_.setGbnActive(arq_.pendingCount() > 0);
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(missingCobsSeq))
        return false;
    arq_.onNaked(missingCobsSeq, hw.nowMs());
    lastNakSeq_ = missingCobsSeq;
    rxBytes += RX_NAK_WIRE_BYTES;

    // In SYNC the ArqCache is never populated, so a NAK-driven cache
    // lookup misses and re-sends a zero-byte frame the peer mistakes
    // for a seq advance. The blocking sendMsg retx ladder owns SYNC
    // recovery instead.
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return false;

    // GBN: only the oldest unacked (the base) can legitimately be
    // "missing" under strict in-order delivery. A NAK for anything
    // else is stale wire noise (the base already advanced); ignore
    // it rather than resending out of the contiguous-window
    // invariant.
    if (arq_.gbnActive() && missingCobsSeq == arq_.gbnBase())
        gbnResendWindow_unlocked(hw.nowMs());

    return false;
}

bool Link::onFrameError() { return err_unlocked(); }

} // namespace autolink
