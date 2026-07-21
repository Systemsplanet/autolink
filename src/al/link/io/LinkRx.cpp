
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/arq/IArqCache.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"
#include "al/link/io/LinkMsgCodec.h"
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
                // Not CTRL: every held byte (the 0xAA included) is
                // COBS payload. A nested CTRL start inside them is
                // lost; retransmit recovers.
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
                    // COBS payload can hold any non-zero byte, so
                    // the 0xAA 0x55 sentinel collides with random
                    // data. Validate the CRC8 before consuming five
                    // bytes as CTRL — a collision consumed as CTRL
                    // fails CRC downstream, counts a frame error and
                    // eventually drops the link. On a CRC fail skip
                    // only the 0xAA so the rest still reaches the
                    // COBS framer.
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
                    // Not CTRL: drop the 0xAA (a valid COBS run-code
                    // byte) and let the framer see the rest.
                    i++;
                    continue;
                }
                if (b == 0xAA && (len - i) < CTRL_FRAME_SIZE &&
                    ((len - i) < 2 || data[i + 1] == 0x55)) {
                    // A CTRL frame split across delivery chunks:
                    // hold the tail. Feeding it to the COBS framer
                    // would corrupt the stream — CTRL bytes may
                    // contain 0x00.
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
    // Settle-drain gate for the SWP-frame / keepalive
    // PING path. The window exists to swallow line
    // garbage from the baud switch — bytes that were
    // mid-flight at the old rate and re-frame into
    // nonsense at the new one. It must therefore gate
    // on validation, not arrive before it: a frame
    // whose CRC8 passes is not baud-switch garbage, it
    // is a real frame from a peer already at the locked
    // baud, and dropping it strands the sender for a
    // full RTO ladder. Order is CRC first, then the
    // window: garbage inside the window is swallowed
    // without counting a frame error (it is expected,
    // not a link fault); garbage outside the window is
    // a genuine error; a CRC-valid frame is processed
    // either way. The stale-peer case the old
    // drop-everything gate guarded against is already
    // covered by the session epoch carried in the seq
    // byte (the OK-state epoch-mismatch resync below).
    // Pinned by SettleGateTest.
    if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) !=
        rxBuf[CTRL_FRAME_CRC_IDX]) {
        if (hw.nowMs() < settleUntilMs_) {
            settleDrops_++;
            Log::log().debug(TAG,
                             "wire CTRL garbage swallowed: post-lock "
                             "settle (until %lu)",
                             (unsigned long)settleUntilMs_);
            return false;
        }
        Log::log().debug(
            TAG, "wire CTRL CRC fail: rx=[%02X %02X %02X %02X %02X]",
            (unsigned)rxBuf[0], (unsigned)rxBuf[1], (unsigned)rxBuf[2],
            (unsigned)rxBuf[3], (unsigned)rxBuf[4]);
        return err_unlocked();
    }
    // CRC-valid CTRL frame receipt: log the wire shape so the
    // SWP-state machine's decisions (PING/PONG/REQ/LOCK) are
    // traceable. Debug, not info: SWP-frame
    // arrival rate can hit the master's PING-tick cadence
    // (every idleTimeoutMs/2 ≈ 2.5 s default), but
    // burst-mode PINGs in the wire noise glitch path can
    // produce N per second. The state-transition log in
    // handleSwp_unlocked and the Locked log are the
    // info-level event markers.
    Log::log().debug(TAG, "wire CTRL seq=%u payload=0x%02X state=%s",
                     (unsigned)rxBuf[CTRL_FRAME_SEQ_IDX],
                     (unsigned)rxBuf[CTRL_FRAME_PAYLOAD_IDX], StateToStr(cur));
    // A CRC-validated CTRL frame is proof the link is
    // exchanging real data at the locked baud. Stamps
    // lastValidRxMs (NOT just lastRxMs — a noise byte
    // that happens to land on a 0xAA 0x55 preamble
    // would stamp lastRxMs but the frame would fail
    // CRC). The health machine's lastRxMs is now
    // shadowed by lastValidRxMs for the rxAge check.
    lastValidRxMs = hw.nowMs();
    uint8_t cs = rxBuf[CTRL_FRAME_SEQ_IDX];
    uint8_t pl = rxBuf[CTRL_FRAME_PAYLOAD_IDX];
    if (cur == State::OK) {
        noteValidFrameOk_unlocked();
        // Sweep PINGs carry sweepEpoch_ in the seq byte, not txSeq.
        // A PING with a different epoch than the one we last
        // observed means the peer restarted under our feet while
        // we stayed OK — auto-ACKing it would walk us through the
        // GAP...dropped storm from a peer that's already on a
        // fresh session. Force a resync (same call onBreak() makes
        // for an inbound break) and let the sweep machinery take
        // it from there. Genuine keepalive PINGs match the
        // latched epoch and fall through to the auto-ack.
        if (pl == PING_CMD && peerSweepEpochKnown_ && cs != peerSweepEpoch_) {
            Log::log().info(TAG,
                            "OK-state PING epoch mismatch: peer %u -> %u, "
                            "forcing resync",
                            (unsigned)peerSweepEpoch_, (unsigned)cs);
            // Recovery vs reboot: every recovery reset bumps the
            // peer's epoch by exactly 1, so a small FORWARD delta
            // means the same peer is resweeping the same logical
            // session — classify as HealthWatchdog so BOTH sides
            // take the session-resume path symmetrically (v1 of
            // session resume failed partly because the still-OK
            // side wiped here while the resweeping side
            // preserved). A reboot restarts the peer's epoch
            // counter: arbitrary/backward delta, genuinely new
            // session, full wipe via PeerEpochMismatch.
            uint8_t epochDelta = (uint8_t)(cs - peerSweepEpoch_);
            peerSweepEpoch_ = cs;
            reset_unlocked(true, /*preservePreferredBaud=*/true,
                           (epochDelta >= 1 && epochDelta <= 8)
                               ? ResetReason::HealthWatchdog
                               : ResetReason::PeerEpochMismatch);
            return true;
        }
        if (pl == PING_CMD) {
            // Latch the epoch on the very first sweep-frame arrival
            // so the mismatch check above is armed for any
            // subsequent PING. A genuine keepalive PING (no
            // resweep in flight on our side, peer is still on
            // its original session) carries the same epoch we
            // latched, so the first PING just learns the epoch
            // and falls through to the plain auto-ack.
            if (!peerSweepEpochKnown_) {
                peerSweepEpoch_ = cs;
                peerSweepEpochKnown_ = true;
            }
            sendPongAck_unlocked();
        }
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
    if (state != State::OK) {
        Log::log().debug(TAG, "wire COBS seq=%u ignored in SWP state (n=%d)",
                         (unsigned)cobsSeq, n);
        return true;
    }
    // No settle-drain gate here. This frame has
    // already cleared COBS decode and its CRC — it is
    // not baud-switch garbage, it is real data from a
    // peer transmitting at the locked baud. Dropping
    // it silently (the old behaviour) stranded the
    // sender: no ACK, no NAK, so the sender ran its
    // RTO ladder into the base-stuck monitor and took
    // an honest link drop while the receiver was
    // deliberately discarding proven-good data. Every
    // chunk of a message sent inside the first
    // AUTOLINK_WIRE_SETTLE_MS after a lock was lost
    // that way, with nothing counted anywhere. The
    // settle window now gates only on failed
    // validation, in processCtrlFrame_unlocked; the
    // framer discards undecodable bytes on its own.
    // Pinned by SettleGateTest.
    noteValidFrameOk_unlocked();
    // A CRC-validated COBS frame is the strongest proof
    // the link is alive at the locked baud — stamp
    // lastValidRxMs so the health machine doesn't read a
    // noise byte as ongoing traffic.
    lastValidRxMs = hw.nowMs();

    lastRxSeq_ = cobsSeq;
    // The Ping-side "wire <seq> <bytes>" companion log already
    // captures the per-chunk-OK rate. Skip duplicating it here
    // at the same level — this onPayload runs at the ASYNC
    // pipeline rate and a debug-level duplicate would double
    // the per-chunk log volume. Verbose, not debug, so even
    // deep-trace operators see one of the two lines (the Ping
    // companion at info, this at verbose), never both.
    // Per-frame trace; default-compiled-out. The ASYNC
    // pipeline rate is one log line per chunk (~400/s in
    // the field) and a verbose-level line at that rate is
    // exactly the transport saturation the the prior shape field
    // log showed. Enable by building with
    // -DAUTOLINK_TRACE_WIRE; the per-second summary counter
    // (Stats log) is the always-on field-side replacement.
    // Pinned by WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "wire COBS ok seq=%u n=%d", (unsigned)cobsSeq, n);
#endif

    int diff = 0;
    GapClass cls = classifyGap(cobsSeq, rxSeq, rxSeqSet, &diff);
    // Duplicate of a delivered frame (its ACK was lost): re-ACK so
    // the sender frees the slot. Wrap-safe because the admission gate
    // keeps every pending seq inside the window.
    if (cls == GapClass::Stale) {
        stale++;
        // Never ACK a frame this side did not actually deliver.
        // "Stale" spans two very different cases in mod-254 seq
        // space: (a) a genuine duplicate of a recently-delivered
        // frame (its ACK was lost; sender retx'd) — re-ACK so the
        // sender frees the slot; (b) a frame more than 2 windows
        // behind the rx cursor — wrap-AMBIGUOUS: it is at least as
        // likely a brand-new frame from a sender whose numbering
        // has diverged (e.g. the sender reset mid-session and
        // restarted txSeq while this side kept its cursor) as an
        // ancient duplicate. Blind re-ACK of case (b) manufactures
        // delivery confirmations for data that went nowhere: the
        // sender's cumulative-ACK walk frees its window, its base
        // advances over the undelivered frames, fresh sends wrap
        // back into the ambiguous band, and the loop self-sustains
        // — a permanent black hole where both link layers look
        // healthy and the app receives nothing (wedge at
        // t=44924: receiver pinned at exp=81 while the sender's
        // base cycled the entire seq space via these manufactured
        // ACKs). Dropping case (b) SILENTLY (no ACK, no NAK) is
        // the honest signal: the sender's base stalls at the first
        // truly-undelivered seq, the storm-immune base-stuck clock
        // (sweepRetx_unlocked) elapses, and the maxRetx honest-drop
        // resets and resyncs both sides — a bounded recovery
        // instead of an unbounded wedge. A genuine-duplicate
        // horizon of 2 windows is unreachable by live traffic
        // (the sender can never legitimately be re-sending
        // anything older than one window). Pinned by
        // StaleAmbiguousNoAckTest.
        int back = LD_SEQ_WRAP - diff; // how far BEHIND expected
        bool genuineDup =
            diff == 0 || back <= 2 * (int)AUTOLINK_ARQ_PIPELINE_WINDOW;
        if (!genuineDup) {
            staleAmbiguous_++;
            Log::log().debug(TAG,
                             "wire COBS seq=%u %d behind exp — wrap-"
                             "ambiguous, dropped without ACK",
                             (unsigned)cobsSeq, back);
            return false;
        }
        sendAckFrame_unlocked(cobsSeq, (uint16_t)(n > 0 ? n : 0));
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
            gaps++;
            lostMsgs += (uint64_t)diff;
            // SYNC gap accept also goes through
            // the all-or-nothing gate. Same
            // rationale as the Forward path: a
            // partial write splices garbage into
            // the message stream. Pinned by
            // AppBufFullAdmitNothingTest.
            if (n > 0 && hw.appBufFree() >= n) {
                int acc = hw.pushAppBuf(b, n);
                if (acc == n) {
                    rxBytes += (uint64_t)acc;
                    sendAckFrame_unlocked(cobsSeq);
                } else {
                    // Defensive: appBufFree said
                    // we'd fit, push took fewer.
                    // NAK so the sender retries
                    // (without rxSeq advance — the
                    // Gap path doesn't touch rxSeq
                    // here).
                    Log::log().warning(TAG,
                                       "seq=%u (gap, SYNC) pushAppBuf "
                                       "accepted %d of %d — NAK",
                                       (unsigned)cobsSeq, acc, n);
                    sendNakFrame_unlocked(cobsSeq);
                }
            } else if (n > 0) {
                Log::log().warning(TAG,
                                   "seq=%u (gap, SYNC) app buf full "
                                   "(want %d free=%d) — NAK",
                                   (unsigned)cobsSeq, n, hw.appBufFree());
                sendNakFrame_unlocked(cobsSeq);
            } else {
                sendAckFrame_unlocked(cobsSeq);
            }
            return false;
        }
        // In-order-only accept: an early frame is dropped, not
        // buffered. NAKing the expected seq on every out-of-order
        // arrival is the only recovery path. Reverted a throttle
        // attempted here: it broke
        // test_multichunk_async_under_loss (sender wedge, RX
        // stalled at 1 for the whole run) — this codebase's
        // recovery protocol is deliberately built around cheap,
        // idempotent, undamped immediate NAKs/resends; any
        // cadence throttle on any NAK path breaks that contract.
        uint8_t exp = reorderExpectedSeq();
        // Hold-induced vs loss-induced gap: if `exp` is a frame we
        // just deliberately HoldAck'd (app buf full — see the
        // HoldAck branch below), it was NOT lost on the wire; the
        // hold's own NAK already told the sender to retx it, and it
        // will be held again until the app drains. Re-NAKing it
        // here once per out-of-order arrival multiplies into a
        // full-window resend per NAK (onNak's inline-resend
        // contract, deliberately undamped — loopback_multichunk_
        // test) — 32 base-NAKs per burst, 32x wire amplification,
        // enough to overflow the channel and corrupt frames
        // mid-wire (WireSimAppBufFullTest: 31 COBS frameErrs).
        // Loss-induced gaps (holdNak inactive) keep the immediate
        // NAK — that recovery contract is untouched. Pinned by
        // HoldGapNakSuppressTest.
        if (holdNakActive_ && exp == holdNakSeq_) {
            // Not counted in gaps/lostMsgs either: those are WIRE
            // diagnostics, and nothing was lost on the wire — the
            // head frame is deliberately held and this one will be
            // re-delivered by the retx the hold's NAK already
            // requested. Pinned by LinkCobsSeqTest's
            // app-buffer-full-doesn't-trip-errThreshold pin.
            Log::log().debug(TAG,
                             "GAP seq=%u exp=%u diff=%d (dropped; NAK "
                             "suppressed: exp is held, not lost)",
                             (unsigned)cobsSeq, (unsigned)exp, diff);
            return false;
        }
        gaps++;
        lostMsgs++;
        Log::log().info(TAG, "GAP seq=%u exp=%u diff=%d (dropped)",
                        (unsigned)cobsSeq, (unsigned)exp, diff);
        sendNakFrame_unlocked(exp);
        return false;
    }
    // In-order (Forward) path. All-or-nothing
    // admission: check the app buf has room for the
    // entire payload BEFORE writing any byte. The
    // buggy-original shape did `acc = hw.pushAppBuf(b, n)`
    // first and NAKed on shortfall, but a partial
    // write spliced garbage into the message
    // stream (120 of 143 bytes
    // pushed, then NAK, then `recv rejected
    // (CRC/desync)` from the message parser). The
    // buggy-original shape also advanced `rxSeq = cobsSeq`
    // BEFORE the HoldAck check, so a NAKed frame's
    // retransmission classified as `Stale`, was
    // re-ACKed, and was dropped — the data was
    // permanently lost. Fix: check free space first,
    // write nothing on NAK, and only commit the
    // seq stamp on a successful delivery. Pinned by
    // AppBufFullAdmitNothingTest.
    if (n == 0) {
        // Empty payload (e.g. an MSG_HDR-only chunk
        // that the message parser will pick up on
        // the next recvMsg call) — commit the seq,
        // clear errs, no buf write needed.
        rxSeq = cobsSeq;
        rxSeqSet = true;
        if (errs > 0)
            errs = 0;
        return false;
    }
    if (hw.appBufFree() < n) {
        // All-or-nothing: app buf can't hold the
        // entire payload. NAK the seq and DO NOT
        // advance rxSeq — the retx will land when
        // the app has drained some bytes, and the
        // retx will classify in-order and deliver.
        Log::log().warning(TAG,
                           "seq=%u app buf full (want %d free=%d) — "
                           "NAK, no write, no seq advance",
                           (unsigned)cobsSeq, n, hw.appBufFree());
        // Reverted a "one NAK per RTO" throttle here: it
        // regressed WireSimAppBufFullTest (link wedged
        // permanently after an app-buf-full stress burst
        // required by WireSimAppBufFullTest). The NAK-storm
        // bad_alloc this was meant to fix only reproduced
        // under the harness's unrealistic unbounded
        // burst-send rate (48 in-flight sends with no
        // pacing); the harness needs a send-side pacing
        // fix, not a change to this recovery contract.
        holdNakActive_ = true;
        holdNakSeq_ = cobsSeq;
        sendNakFrame_unlocked(cobsSeq);
        return false;
    }
    int acc = hw.pushAppBuf(b, n);
    if (acc != n) {
        // Defensive: appBufFree() said we'd fit
        // but pushAppBuf() accepted fewer. Don't
        // commit the seq stamp, NAK so the sender
        // re-sends the chunk (we may have just
        // observed an aborted write under
        // concurrent push from another code path).
        Log::log().warning(TAG,
                           "seq=%u pushAppBuf accepted %d of %d "
                           "(appBufFree=%d pre-write) — NAK, no "
                           "seq advance",
                           (unsigned)cobsSeq, acc, n, hw.appBufFree());
        sendNakFrame_unlocked(cobsSeq);
        return false;
    }
    rxBytes += (uint64_t)acc;
    // The held frame finally landed: hold-induced gaps are over,
    // any future gap at the next expected seq is genuine wire loss
    // and must NAK normally again.
    if (holdNakActive_ && cobsSeq == holdNakSeq_) {
        holdNakActive_ = false;
        holdNakSeq_ = 0xFF;
    }
    // Commit the seq stamp only on a fully-accepted
    // delivery. The retx of a previously-NAKed
    // frame now classifies in-order (Forward),
    // delivers, and the seq advances exactly
    // once. The buggy-original shape's "advance first,
    // NAK second" left the seq at the NAKed
    // cobsSeq so the retx classified as Stale and
    // was re-ACKed away — the message was lost
    // despite NAK+retx (run A's wedge).
    rxSeq = cobsSeq;
    rxSeqSet = true;
    sendAckFrame_unlocked(cobsSeq, (uint16_t)n);
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    if (state != State::OK) {
        Log::log().debug(TAG, "wire ACK seq=%u ignored in SWP state",
                         (unsigned)ackedCobsSeq);
        return false;
    }
    // A CRC-validated ACK proves the peer hears us at the locked
    // baud exactly as strongly as a delivered message does — the
    // frameRx feed only dispatches here on CRC pass. Without this,
    // a unidirectional sender (peer ACKs but never sends data, so
    // recvMsg never clears the counter) accumulates
    // locksWithoutRecv_ on every preserved relock until
    // kPeerBaudMismatchThreshold permanently vetoes the preserved
    // fast path — every honest-drop recovery then walks P1 from
    // 9600 into the master-walks/slave-camps deadlock (fieldsoak:
    // deterministic mutual-SWP wedge at t=44924 despite
    // preserve=1 on the GbnMaxRetx reset). Pinned by the
    // fieldsoak acceptance gate.
    locksWithoutRecv_ = 0;
    // No settle-drain gate here. A pre-lock ACK cannot
    // advance this session's gbnBase in any case: the
    // reset that preceded the lock ran arq_.clearAll(),
    // so nothing from the old generation is pending and
    // the isPending check below rejects it. Dropping
    // validated ACKs for the settle window instead
    // stranded in-flight chunks whose ACKs happened to
    // land inside it. Pinned by SettleGateTest.
    if (!arq_.isPending(ackedCobsSeq))
        return false;
    // A CRC-valid ACK frame is proof of a live peer
    // even if no app data is moving in this
    // direction. Stamp lastValidRxMs so the health
    // machine's rxAge clock refreshes on ACK-only
    // stretches (all-outbound-data window, echoes
    // delayed) — without this, the watchdog reads
    // an ACK-only stretch as RX silence and
    // DropAsymIdle fires mid-repair. Pinned by
    // LastValidRxMsTest.
    lastValidRxMs = hw.nowMs();
    lastAckSeq_ = ackedCobsSeq;
    recentDiscs_ = 0;
    rxBytes += RX_ACK_WIRE_BYTES;
    // ACK is the same proof of life as a data frame for
    // the BREAK confirm window: a CRC-valid inbound
    // frame at the locked baud, exactly what would clear
    // the suspicion on an outbound-heavy stream where
    // ACKs are the only inbound traffic. Without this
    // call, an ACK-only stretch lets a spurious BREAK
    // always confirm. Pinned by AckClearsBreakWindowTest.
    noteValidFrameOk_unlocked();

    if (cfg.mode == AutoLinkConfig::Mode::SYNC || !arq_.gbnActive()) {
        // Single-chunk lockstep or an inactive pipeline: at most
        // one slot is ever touched here, and isPending() above
        // already gates it. The wraparound-aliasing risk below
        // only exists for the cumulative range walk.
        arq_.onAcked(ackedCobsSeq, bytesRecvd);
        arqCache_.freeBySeq(ackedCobsSeq);
        return false;
    }

    // isPending()/budgetIdx() key on seq % ARQ_CHUNK_BUDGET, so a
    // stale re-ACK from a prior lap around the 256-value cobsSeq
    // space (seq differing from a currently-live chunk by a
    // multiple of ARQ_CHUNK_BUDGET) passes that check even though
    // it names nothing in the live window. Walking the cumulative
    // range from gbnBase() to such a seq sweeps almost the entire
    // seq space, freeing every in-flight chunk's ArqCache pool
    // slot while arq_'s own pending/base bookkeeping is untouched
    // — base then points at a slot whose bytes are gone, and
    // retxSeq_unlocked's cache-miss path resends nothing for it
    // forever (the fieldsoak's t=22715 permanent GBN wedge).
    // idxOf() is base-relative and window-bounded, so it can't
    // alias: reject anything outside the live window here, before
    // the walk, rather than trying to make the walk itself safe
    // against a range it should never have been asked to cover.
    // Pinned by ArqStaleAckWrapTest.
    if (arq_.idxOf(ackedCobsSeq) < 0) {
        Log::log().warning(
            TAG,
            "wire ACK seq=%u base=%u — outside live window, stale "
            "prior-lap re-ACK ignored",
            (unsigned)ackedCobsSeq, (unsigned)arq_.gbnBase());
        return false;
    }

    // Cumulative: ackedCobsSeq is the receiver's new contiguous
    // high-water mark, so free every slot from the base through it.
    // Interior slots — whose own ACK never arrived, which is why the
    // base was stuck — are backfilled from our own cache, since the
    // cumulative ACK proves they landed byte-for-byte.
    uint8_t s = arq_.gbnBase();
    int freed = 0;
    for (;;) {
        // Invariant tripwire: a bounded walk (guarded above) can
        // never exceed one window's worth of slots. If it does,
        // idxOf()/gbnBase() have desynced — stop rather than keep
        // freeing into the unknown, and surface it loudly.
        if (++freed > (int)AUTOLINK_ARQ_PIPELINE_WINDOW) {
            Log::log().warning(TAG,
                               "wire ACK cumulative walk exceeded window "
                               "(base=%u acked=%u) — aborting, invariant "
                               "violated",
                               (unsigned)arq_.gbnBase(),
                               (unsigned)ackedCobsSeq);
            break;
        }
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
    consecutiveKeep_ = 0;
    arq_.setGbnActive(arq_.pendingCount() > 0);
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq) {
    if (state != State::OK) {
        Log::log().debug(TAG, "wire NAK missing=%u ignored in SWP state",
                         (unsigned)missingCobsSeq);
        return false;
    }
    // Same baud-proof rationale as onAck above: a CRC-valid NAK
    // is a valid crossing at the locked baud.
    locksWithoutRecv_ = 0;
    // No settle gate here: a NAK for a pre-lock
    // seq is a no-op (arq_.isPending returns false),
    // and a NAK for a current-session seq must be
    // honored immediately so the inline retx path
    // fires before the OK-timer tick. Pinned by
    // LinkFastRetxTest.
    if (!arq_.isPending(missingCobsSeq))
        return false;
    // A CRC-valid NAK frame is also proof of a live
    // peer — a dead peer doesn't bother NAKing.
    // Stamp lastValidRxMs for the same reason onAck
    // does: an ACK/NAK-only stretch (no app
    // payloads, peer-side backpressure) is exactly
    // the shape that DropAsymIdle mis-read as
    // silence. Pinned by LastValidRxMsTest.
    lastValidRxMs = hw.nowMs();
    arq_.onNaked(missingCobsSeq, hw.nowMs());
    lastNakSeq_ = missingCobsSeq;
    rxBytes += RX_NAK_WIRE_BYTES;

    // SYNC never populates the ArqCache, so a NAK-driven lookup would
    // miss and resend a zero-byte frame the peer reads as a seq
    // advance. sendMsg's blocking retx ladder owns SYNC recovery.
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return false;

    // Under in-order delivery only the base can legitimately be
    // missing. A NAK for anything else is stale wire noise.
    //
    // Reverted a "one resend per RTO" damping attempt here: it
    // broke loopback_multichunk_test's explicit contract (see
    // that file's header) that a NAK triggers an immediate,
    // undamped full-window resend — with damping, delivery under
    // loss collapsed and the sender wedged. The "ARQ retx
    // verbatim" storm this was meant to fix is real under the
    // fieldsoak's synthetic bounded-buffer + slow-drain condition,
    // but the fix needs to be receiver-side NAK throttling (which
    // stayed in onPayload's app-buf-full branch below), not a
    // sender-side cap on honoring NAKs it does receive.
    if (arq_.gbnActive() && missingCobsSeq == arq_.gbnBase())
        gbnResendWindow_unlocked(hw.nowMs());

    return false;
}

bool Link::onFrameError() { return err_unlocked(); }

} // namespace autolink
