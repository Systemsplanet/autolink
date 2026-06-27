// LinkRx -- onRx (the UART-byte dispatcher), processCtrlFrame_unlocked,
// ctrlFrameReady_unlocked, onPayload (UtilFrameRx listener), onAck /
// onNak / onFrameError, findMsgHeaderResync_unlocked, recvMsg.
//
// The receive path owns two listeners:
//   - onRx() is the byte-stream entry point; it demuxes
//     CTRL frames (0xAA 0x55 ...) from data bytes and
//     feeds the latter to UtilFrameRx.
//   - onPayload / onAck / onNak / onFrameError are
//     the UtilFrameRx listener callbacks; they run
//     inside the link lock.
//
// findMsgHeaderResync_unlocked walks a window of the
// receive app buffer to recover from a corrupted MSG_HDR.
// recvMsg (the public API) lives in LinkApi.cpp.
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/arq/IArqCache.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"
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
    for (int d = 0; d + MSG_HDR <= got; d++) {
        uint32_t L = (uint32_t)snap[d] | ((uint32_t)snap[d + 1] << 8) |
            ((uint32_t)snap[d + 2] << 16) | ((uint32_t)snap[d + 3] << 24);
        if (L < 1 || L > cfg.maxMsg)
            continue;
        if (d + (int)L + 6 > got)
            continue;
        uint16_t wc = (uint16_t)snap[d + 4] | ((uint16_t)snap[d + 5] << 8);
        if (UtilCrc::crc16(snap + d + 6, (int)L) != wc)
            continue;
        hw.pushAppBuf(snap + d, got - d);
        free(snap);
        return d;
    }
    // No valid header found in the scan
    // window. The buffer is unrecoverable
    // garbage — drop everything, not just
    // the snap. Pushing the snap back
    // keeps the desync alive: every
    // subsequent recvMsg() call would
    // re-scan the same noise and re-fail,
    // and any bytes that were in the
    // buffer beyond the scan window would
    // still be there waiting to confuse
    // the next valid frame. Drain the
    // whole appBuf so the next valid
    // frame on the wire (which the peer's
    // retransmit will produce) has a
    // clean buffer to land in.
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
            int start = i;
            while (i < len) {
                uint8_t b = data[i];
                if (b == 0xAA && (len - i) >= 2 && data[i + 1] == 0x55) {
                    if (i > start) {
                        int c = frameRx.feed(data + start, i - start);
                        if (state != State::OK) {
                            i = start + c;
                            break;
                        }
                    }
                    if (len - i >= CTRL_FRAME_SIZE) {
                        for (int k = 0; k < CTRL_FRAME_SIZE; k++)
                            rxBuf[k] = data[i + k];
                        i += CTRL_FRAME_SIZE;
                        rxIdx = 0;
                        if (processCtrlFrame_unlocked(cur))
                            needBreak = true;
                        start = i;
                    } else {
                        int c = frameRx.feed(data + start, len - start);
                        i = start + c;
                        start = i;
                        break;
                    }
                } else {
                    i++;
                }
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
        // Heartbeat removed in this release. The OK-state
        // PING/PONG handling stays as a wire-level
        // "I heard you" reply so a peer still running
        // the legacy heartbeat probe sees a PONG back,
        // but no peer-miss bookkeeping is kept. The
        // PING_CMD / PONG_CMD constants stay in
        // LinkContext.h because SWP-state code still
        // sends PING/PONG during the sweep phases.
        if (pl == PING_CMD)
            sendPongAck_unlocked();
        return false;
    }
    return ctrlFrameReady_unlocked(cs, pl, cur);
}

bool Link::ctrlFrameReady_unlocked(uint8_t cs, uint8_t pl, State cur) {
    if (cur == State::SWP)
        return handleSwp_unlocked(cs, pl);
    if (cur == State::LCK)
        return handleLck_unlocked(cs, pl);
    return false;
}

bool Link::onPayload(uint8_t cobsSeq, const uint8_t *b, int n) {
    if (state != State::OK)
        return true;
    // Stamp the seq of the most recently received data
    // frame so Pong's diagnostic ack log can name the
    // seq without forcing Pong to expose Link::onPayload
    // directly. Cleared in reset_unlocked.
    lastRxSeq_ = cobsSeq;
    // Drop expired reorder slots before
    // classifying this frame. lostMsgs is
    // incremented here, on the per-slot
    // expiry, so a retransmit that lands
    // in time doesn't count as lost.
    int dropped = reorder_.dropExpired(hw.nowMs(), cfg.reorderHoldMs);
    if (dropped > 0)
        lostMsgs += (uint64_t)dropped;
    int diff = 0;
    GapClass cls = classifyGap(cobsSeq, rxSeq, rxSeqSet, &diff);
    if (cls == GapClass::Stale) {
        stale++;
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
            // Sender waits for ack before
            // sending the next frame, so
            // gaps shouldn't happen. If they
            // do, the wire has reordered or
            // duplicated something; drop it
            // and ACK so the sender's pool
            // frees up. No reorder buffer
            // use, no malloc.
            gaps++;
            lostMsgs += (uint64_t)diff;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }
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
        uint8_t exp = reorderExpectedSeq();
        gaps++;
        // Don't bump lostMsgs here. The
        // out-of-order frame is held in
        // the reorder buffer waiting for
        // the missing seq; if the
        // retransmit arrives within
        // reorderHoldMs, the frame is
        // delivered in-order and nothing
        // was lost. lostMsgs is only
        // incremented when the reorder
        // buffer expires (see
        // reorderDropExpired_unlocked).
        Log::log().info(TAG, "GAP seq=%u exp=%u diff=%d", (unsigned)cobsSeq,
                        (unsigned)exp, diff);
        if (n > 0) {
            bool newlyHeld = reorder_.hold(cobsSeq, b, n, hw.nowMs());
            if (!newlyHeld) {
                // hold() returns false on OOM
                // (malloc failure) too. Drop
                // the slot and bump lostMsgs.
                lostMsgs++;
                rxSeq = cobsSeq;
                rxSeqSet = true;
                return false;
            }
        } else {
            // Zero-len (keepalive) out of
            // order: don't reserve a reorder
            // slot. A null-buf in_use slot
            // would cause pushAppBuf(null,0)
            // and a spurious ACK advancing
            // rxSeq past the real gap.
            lostMsgs++;
        }
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
        // App buffer is full (peer is sending faster than
        // the app can drain). The pre-fix code logged and
        // returned true, which signaled feed() to drop the
        // rest of the byte stream with no wire-side
        // indication — the sender kept re-issuing the same
        // seq and the wire went quiet from the sender's
        // point of view. Now we send a NAK so the sender
        // knows the frame wasn't accepted, the ARQ timer
        // restamps sentAtMs_ (see LinkArq::onNaked), and
        // the sender's retransmit loop brings this chunk
        // back once the app buffer has room.
        Log::log().warning(
            TAG,
            "seq=%u app buf full "
            "(want %d got %d) — sending NAK",
            (unsigned)cobsSeq, n, acc);
        sendNakFrame_unlocked(cobsSeq);
        return false;
    }
    sendAckFrame_unlocked(cobsSeq, (uint16_t)n);
    reorder_.flushContiguous(*this, hw.nowMs());
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(ackedCobsSeq))
        return false;
    arq_.onAcked(ackedCobsSeq);
    arqCache_.freeBySeq(ackedCobsSeq);
    bytesRecvd_[ackedCobsSeq] = bytesRecvd;
    lastAckSeq_ = ackedCobsSeq;
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(missingCobsSeq))
        return false;
    arq_.onNaked(missingCobsSeq, hw.nowMs());
    pendingRetxBase_ = missingCobsSeq;
    hasPendingRetx_ = true;
    lastNakSeq_ = missingCobsSeq;
    return false;
}

bool Link::onFrameError() { return err_unlocked(); }

} // namespace autolink
