
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/arq/IArqCache.h"
#include "al/util/log/Log.h"
#include "al/util/codec/UtilCrc.h"
#include "al/link/io/LinkMsgCodec.h"
#include <stdlib.h>

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

// AL97-5: return value now distinguishes "couldn't scan at all"
// (-1: avail < MSG_HDR, or malloc failure — no bytes touched) from
// "scanned and found no valid header" (>= 0: the count of bytes
// actually discarded). The old shape returned -1 for both, so a
// caller logging "resync scan dropped %d bytes" printed a
// nonsensical negative byte count for the (overwhelmingly more
// common) second case, and no counter anywhere recorded how much
// data a desync event actually cost.
//
// The failure path's hw.clearAppBuf() wipes the ENTIRE app buffer
// (avail bytes), not just the snapLen-capped scan window (got
// bytes) — avail > got is possible whenever max_scan caps the scan
// below what's actually queued. Reporting `got` there would
// undercount the true loss; `avail` is what was actually destroyed.
// Pinned by ResyncScanReportsDroppedBytesTest.
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
    resyncDroppedBytes_ += (uint64_t)avail;
    return avail;
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
        return err_unlocked(FrameErrCause::CrcFail);
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
    // the field) and a verbose-level line at that rate
    // would flood the wire log. Enable by building with
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
        // Always-on log for the Stale re-ACK path: the
        // code was silent here, so an operator
        // could not distinguish "peer never received the
        // retx" from "peer received it and re-ACKed."
        // Pinned by StaleReAckDebugTest.
        Log::log().debug(TAG, "wire COBS seq=%u dup (exp=%u) — re-ACK",
                         (unsigned)cobsSeq, (unsigned)rxSeq);
        sendAckFrame_unlocked(cobsSeq, (uint16_t)(n > 0 ? n : 0));
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
            // AL87-10: gaps/lostMsgs are wire-loss diagnostics —
            // "the sender put frames on the wire we never saw".
            // Backpressure (this side's own app buf is full) is
            // not wire loss; the frame is sitting on the wire (or
            // about to be retransmitted) and WILL be delivered
            // once the app drains. Counting it here inflated
            // lostMsgs for every held frame, matching the same
            // conflation ASYNC's Forward/Gap paths already avoid
            // via holdNakActive_ (see HoldGapNakSuppressTest) —
            // SYNC's Gap branch was the one path that still got
            // this wrong. Only count once the disposition is
            // known NOT to be an app-buf-full hold. Pinned by
            // SyncGapBackpressureNotCountedAsLossTest.
            //
            // SYNC gap accept also goes through
            // the all-or-nothing gate. Same
            // rationale as the Forward path: a
            // partial write splices garbage into
            // the message stream. Pinned by
            // AppBufFullAdmitNothingTest.
            if (n > 0 && hw.appBufFree() >= n) {
                int acc = hw.pushAppBuf(b, n);
                if (acc == n) {
                    gaps++;
                    lostMsgs += (uint64_t)diff;
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
                gaps++;
                lostMsgs += (uint64_t)diff;
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
        if (holdNakActive_ && exp == holdNakSeq_ &&
            rxSeqWrap_ == holdNakWrap_) {
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
        // AL89-5: hold NAK is now self-describing. The
        // previous shape NAKed the held seq on every
        // retx arrival, multiplying into a
        // full-window resend per NAK (the peer's
        // onNak inline-resend is deliberately
        // undamped — see loopback_multichunk_test).
        // Throttling the NAK itself broke the
        // recovery contract (WireSimAppBufFullTest
        // regressed), so the new shape keeps the
        // NAK-on-retx behavior but re-emits only
        // when the receiver's appBufFree has
        // actually grown since the hold was set —
        // the only signal that the peer drain has
        // made real progress. A peer that isn't
        // draining at all is blocked, not lossy;
        // repeated NAKs amplify the wire into the
        // 29-NAKs-per-base storm seen in the field
        // capture. Loss-induced NAKs (the Gap
        // path's `sendNakFrame_unlocked(exp)`
        // above) are unchanged. Pinned by
        // HoldNakSelfDescribingTest.
        bool freshHold = !holdNakActive_ || holdNakSeq_ != cobsSeq ||
            holdNakWrap_ != rxSeqWrap_;
        // AL97-2: re-emit on drain progress (unchanged) OR once per
        // HOLD_NAK_LIVENESS_MS with zero drain progress. A receiver
        // whose app never drains at all (not slow — stalled) never
        // satisfies the appBufFree()-grew test, so the fresh-hold NAK
        // was the last thing the sender ever heard from it; the
        // sender's peer-stalled watchdog (baud-derived, 2000 ms
        // floor) then reads that silence as a dead peer and tears
        // down a link that was correctly, deliberately backpressuring.
        // The liveness re-emit is capped at one per interval — same
        // amplification bound as the drain-triggered path (still not
        // one NAK per retx). Pinned by HoldNakLivenessCadenceTest.
        uint32_t now = hw.nowMs();
        bool liveness = !freshHold &&
            (uint32_t)(now - holdNakLastMs_) >= HOLD_NAK_LIVENESS_MS;
        if (freshHold) {
            holdNakActive_ = true;
            holdNakSeq_ = cobsSeq;
            // Qualify the held seq with the current rxSeq
            // wrap count so a wrap-around cobsSeq doesn't
            // suppress a real loss-NAK on a prior lap (the
            // same wrap-aliasing class the lastNakSeq_
            // fix closed in. Pinned by
            // HoldGapNakSuppressWrapTest.
            holdNakWrap_ = rxSeqWrap_;
            holdNakFreeAtSet_ = hw.appBufFree();
            holdNakLastMs_ = now;
            sendNakFrame_unlocked(cobsSeq);
        } else if (hw.appBufFree() > holdNakFreeAtSet_) {
            // App has drained some bytes — refresh
            // the snapshot and re-emit one NAK.
            // Still one NAK per fresh-drain, not
            // one NAK per retx.
            holdNakFreeAtSet_ = hw.appBufFree();
            holdNakLastMs_ = now;
            sendNakFrame_unlocked(cobsSeq);
        } else if (liveness) {
            holdNakLastMs_ = now;
            holdNaksLiveness_++;
            sendNakFrame_unlocked(cobsSeq);
        }
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
    if (holdNakActive_ && cobsSeq == holdNakSeq_ &&
        rxSeqWrap_ == holdNakWrap_) {
        holdNakActive_ = false;
        holdNakSeq_ = 0xFF;
        // AL89-5: clear the hold-NAK drain
        // snapshot too. A future hold on a
        // fresh seq starts from the current
        // appBufFree, not the stale value
        // from the just-resolved hold.
        holdNakFreeAtSet_ = 0;
        // AL97-2: clear the liveness stamp alongside — a future
        // hold on a fresh seq starts its own liveness window, not
        // whatever was left ticking from the just-resolved hold.
        holdNakLastMs_ = 0;
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
    // rxSeq wrap detection: cobsSeq went from a
    // high value to a low value. Bump
    // rxSeqWrap_ so holdNakSeq_'s lap qualifier
    // invalidates on the new lap. Pinned by
    // HoldGapNakSuppressWrapTest.
    if (rxSeqSet && cobsSeq < rxSeq &&
        (uint8_t)(rxSeq - cobsSeq) > (uint8_t)COBS_SEQ_SPACE / 2)
        rxSeqWrap_++;
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
    // Per-ACK dispatch shape: SYNC/inactive-GBN frees a
    // single slot, ASYNC cumulative free walks the
    // range. The wire-level always-on info log lives
    // here, outside the dispatch, so a wire ACK is
    // always observable. Pinned by AckReceivedDebugTest.
    //
    // AL97-7: per-ACK verbose trace, same shape as the per-chunk
    // "wire COBS ok" trace above — default-compiled-out. At 512000
    // baud the ACK rate is ~600/s; this line plus its ArqCache-free
    // and GBN-base companions were already demoted from unconditional
    // to verbose (AL-14, see below) to stop flooding a default-level
    // log, but a field session that turns verbose ON *specifically to
    // diagnose a wire-level problem* — exactly the scenario that
    // needs these lines — still floods the 128-entry ring at this
    // rate and loses the state-transition lines around the actual
    // failure (a captured run lost its final 5 seconds this way,
    // right where the link went down). AUTOLINK_TRACE_WIRE is the
    // same compile-time opt-in the per-chunk trace above already
    // uses; the always-on field-side replacement is the aggregate
    // logged once per second in onTimerOk_unlocked. Pinned by
    // AckPathNotVerboseByDefaultTest.
    //
    // Verbose, not debug. At 512000 baud the ACK rate is
    // ~600/s; this line plus its ArqCache-free and GBN-base
    // companions were a 3-line-per-ACK flood that saturated
    // the log sink and dropped the state-transition lines
    // that actually mattered for triage (field set level=4
    // (debug) for a run and lost exactly those lines). Default
    // is now verbose (=5); a deep-dive session opts in. Pinned
    // by PostSoakFieldFixesTest (AL-14 pin).
    //
    // base is formatted through gbnBaseStrForLog so a SYNC
    // session prints "N/A" instead of a constant 0 every line
    // (SYNC never advances gbnBase_). Pinned by
    // PostSoakFieldFixesTest (AL-10 pin).
    wireAckAggAcks_++;
#ifdef AUTOLINK_TRACE_WIRE
    {
        char baseBuf[8];
        Log::log().verbose(
            TAG, "wire ACK seq=%u (base=%s, pending=%d, bytesRecvd=%u)",
            (unsigned)ackedCobsSeq,
            arq_.gbnBaseStrForLog(baseBuf, sizeof(baseBuf)),
            arq_.pendingCount(), (unsigned)bytesRecvd);
    }
#endif
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
    // A CRC-valid ACK frame is proof of a live peer even if no
    // app data is moving in this direction. Stamp lastValidRxMs
    // so the health machine's rxAge clock refreshes on ACK-only
    // stretches (all-outbound-data window, echoes delayed) —
    // without this, the watchdog reads an ACK-only stretch as RX
    // silence and DropAsymIdle fires mid-repair. Pinned by
    // LastValidRxMsTest.
    //
    // This stamp lives ABOVE the isPending() early return below.
    // A CRC-valid ACK for a non-pending seq (a duplicate from a
    // wraparound, or a re-ACK the peer sent after we already
    // freed the slot) is still proof of a live peer at the
    // locked baud — the frameRx feed only dispatches into onAck
    // at all on CRC pass. The prior placement below the return
    // meant the watchdog's rxAge clock failed to refresh on
    // exactly an ACK-only-reject stretch, which is the shape
    // DropAsymIdle mis-read as silence. Pinned by
    // AckNotPendingStillStampsRxAgeTest.
    lastValidRxMs = hw.nowMs();
    // AL89-9: clear the first-lock evidence gate
    // on the first peer ACK observed after
    // postLockFirstTxDone_. A peer that answers
    // at all is live at the locked baud; the
    // settle window has closed; bulk admission
    // can resume. Same shape for NAKs (any
    // CRC-valid frame from the peer counts).
    if (postLockFirstTxDone_ != 0 && postLockFirstTxDone_ != lockedAtMs_) {
        firstPeerResponseSeen_ = true;
    }
    if (!arq_.isPending(ackedCobsSeq)) {
        // Always-on log so a wire-level ACK arriving for a
        // non-pending seq is traceable, not silent (the
        // field log had zero ACK-arrival lines,
        // which is why ACK-loss triage was a dead end).
        // Pinned by AckNotPendingDebugTest.
        char baseBuf2[8];
        Log::log().debug(TAG, "wire ACK seq=%u not pending (base=%s) — ignored",
                         (unsigned)ackedCobsSeq,
                         arq_.gbnBaseStrForLog(baseBuf2, sizeof(baseBuf2)));
        return false;
    }
    lastAckSeq_ = ackedCobsSeq;
    // Release the NAK latch once the seq it names is acked.
    // lastNakSeq_ is otherwise only cleared by reset_unlocked, so
    // on a session that never disconnects it holds the last NAKed
    // seq forever — Ping's gap-stop then re-enters on that stale
    // value every time cobsSeq wraps back around to it and the
    // slot is briefly pending again (field: 4 x 5 s app stalls on
    // seq=189 with disc=0). Cleared here rather than in the
    // cumulative walk below so SYNC and inactive-GBN paths, which
    // return before that walk, get the same release.
    if (lastNakSeq_ == ackedCobsSeq)
        lastNakSeq_ = 0xFF;
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
        arqCache_.freeBySeq(ackedCobsSeq, IArqCache::FreeCause::SingleAck);
        return false;
    }

    // isPending()/budgetIdx() key on seq % ARQ_CHUNK_BUDGET, so a
    // stale re-ACK from a prior lap around the 254-value cobsSeq
    // space (COBS_SEQ_SPACE; the wire's actual wrap, derived from
    // COBS_SEQ_MAX=0xFD plus the 0xFE/0xFF control-frame
    // discriminators) — seq differing from a currently-live chunk
    // by a multiple of ARQ_CHUNK_BUDGET — passes that check even
    // though it names nothing in the live window. Walking the
    // cumulative range from gbnBase() to such a seq sweeps almost
    // the entire seq space, freeing every in-flight chunk's
    // ArqCache pool slot while arq_'s own pending/base bookkeeping
    // is untouched — base then points at a slot whose bytes are
    // gone, and retxSeq_unlocked's cache-miss path resends
    // nothing for it forever (the fieldsoak's t=22715 permanent
    // GBN wedge). idxOf() is base-relative and window-bounded
    // over the same 254-value wire space, so it can't alias:
    // reject anything outside the live window here, before the
    // walk, rather than trying to make the walk itself safe
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

    // AL97-7: oldBase is sampled only for the AUTOLINK_TRACE_WIRE
    // verbose line below; declaring it unconditionally left it
    // unused (and warned on) once that line was gated. See
    // AckPathNotVerboseByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    uint8_t oldBase = arq_.gbnBase();
#endif
    int freed = 0;
    uint8_t s = arq_.gbnBase();
    bool walkOk = true;
    for (;;) {
        if (++freed > (int)AUTOLINK_ARQ_PIPELINE_WINDOW) {
            Log::log().warning(TAG,
                               "wire ACK cumulative walk exceeded window "
                               "(base=%u acked=%u) — aborting, invariant "
                               "violated",
                               (unsigned)arq_.gbnBase(),
                               (unsigned)ackedCobsSeq);
            walkOk = false;
            break;
        }
        uint16_t bytes = bytesRecvd;
        if (s != ackedCobsSeq) {
            const uint8_t *buf = nullptr;
            int len = 0;
            bytes = arqCache_.peekForRetx(s, &buf, &len) ? (uint16_t)len : 0;
        }
        arq_.onAcked(s, bytes);
        arqCache_.freeBySeq(s, IArqCache::FreeCause::CumulativeBackfill);
        if (lastNakSeq_ == s)
            lastNakSeq_ = 0xFF;
        if (s == ackedCobsSeq)
            break;
        s = (s == COBS_SEQ_MAX) ? 0 : (uint8_t)(s + 1);
    }
    if (walkOk) {
        arq_.setGbnBase(
            ackedCobsSeq == COBS_SEQ_MAX ? 0 : (uint8_t)(ackedCobsSeq + 1));
    }
    // AL97-7: same as oldBase above — only needed for the
    // AUTOLINK_TRACE_WIRE verbose line.
#ifdef AUTOLINK_TRACE_WIRE
    uint8_t newBase = arq_.gbnBase();
#endif
    // Always-on log for the base move — a name collision
    // with applyRetx's `base=` field had been hiding this
    // for a long time. Pinned by GbnBaseMoveDebugTest.
    // Verbose, same rationale as the per-ACK line above: GBN
    // base moves are 1-per-ACK, and the per-ACK line is already
    // verbose, so this companion is too. Pinned by
    // PostSoakFieldFixesTest (AL-14 pin).
    // AL97-7: same AUTOLINK_TRACE_WIRE gate as the per-ACK line
    // above, same reason — this is a 1-per-ACK companion to it, so
    // gating one without the other halves the flood, not fixes it.
    // The base-delta this line reports is part of the 1 Hz
    // aggregate (see wireAckAggFreed_ below). Pinned by
    // AckPathNotVerboseByDefaultTest.
    wireAckAggFreed_ += (uint64_t)freed;
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "GBN base %u -> %u (acked=%u freed=%d pending=%d)",
                       (unsigned)oldBase, (unsigned)newBase,
                       (unsigned)ackedCobsSeq, freed, arq_.pendingCount());
#endif
    gbnAttempts_ = 0;
    gbnBackoffMs_ = 0;
    gbnLastRetxBase_ = 0xFF;
    gbnLastResendBase_ = 0xFF;
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
    // Per-NAK dispatch shape: SYNC just wakes the
    // retx ladder (the SYNC path does not free
    // anything from the GBN; see the field comment
    // below). ASYNC/GBN advances the base the same
    // way a cumulative ACK does (NAK(N) means
    // "everything < N landed" in ASYNC's
    // in-order-only delivery), then triggers an
    // undamped resend unless the NAK describes the
    // same loss event as the prior resend.
    // Always-on log for the wire-level arrival —
    // thepath had zero per-NAK lines,
    // which is why "did the NAK arrive, or was it
    // dropped?" could not be answered from a field
    // log. Pinned by NakReceivedDebugTest.
    // Verbose, not debug — same rationale as the per-ACK line in
    // onAck: NAKs are rare in steady state, but a NAK storm is
    // exactly the flood shape that saturated the log sink.
    // Pinned by PostSoakFieldFixesTest (AL-14 pin).
    {
        char baseBufN[8];
        Log::log().verbose(TAG, "wire NAK missing=%u (base=%s, pending=%d)",
                           (unsigned)missingCobsSeq,
                           arq_.gbnBaseStrForLog(baseBufN, sizeof(baseBufN)),
                           arq_.pendingCount());
    }
    // Same baud-proof rationale as onAck above: a CRC-valid NAK
    // is a valid crossing at the locked baud.
    locksWithoutRecv_ = 0;
    // Symmetry with onAck's clear (LinkRx.cpp, inside onAck) and
    // the CRC-valid-delivery clear in LinkApi.cpp: a valid NAK is
    // the same baud-is-good evidence, and a receive-only peer that
    // only ever NAKs (never ACKs) needs this path too.
    recentDiscs_ = 0;
    // A CRC-valid NAK frame is also proof of a live peer — a dead
    // peer doesn't bother NAKing. Stamp lastValidRxMs for the
    // same reason onAck does: an ACK/NAK-only stretch (no app
    // payloads, peer-side backpressure) is exactly the shape that
    // DropAsymIdle mis-read as silence. Pinned by
    // LastValidRxMsTest.
    //
    // Stamp lives ABOVE the isPending() early return, for the
    // same reason as onAck's move: a CRC-valid NAK for a
    // non-pending seq (a re-NAK of a slot that already moved on)
    // is still proof of a live peer. Pinned by
    // NakNotPendingStillStampsRxAgeTest.
    lastValidRxMs = hw.nowMs();
    // AL90-3: clear the first-lock evidence
    // gate on the first peer NAK observed
    // after postLockFirstTxDone_, mirroring
    // onAck's clear. A peer that answers
    // with NAKs is alive at the locked baud
    // (a dead peer doesn't bother NAKing) and
    // the settle window has closed. The
    // previous code cited "any CRC-valid
    // frame" in the comment but only ever
    // cleared on ACK — a blocked peer that
    // answers only with NAKs (the field
    // capture's exact shape) never cleared
    // the gate. Pinned by
    // FirstLockAdmissionEvidenceGateTest.
    if (postLockFirstTxDone_ != 0 && postLockFirstTxDone_ != lockedAtMs_) {
        firstPeerResponseSeen_ = true;
    }
    // No settle gate here: a NAK for a pre-lock
    // seq is a no-op (arq_.isPending returns false),
    // and a NAK for a current-session seq must be
    // honored immediately so the inline retx path
    // fires before the OK-timer tick. Pinned by
    // LinkFastRetxTest.
    if (!arq_.isPending(missingCobsSeq)) {
        // Always-on log for the silent reject. Pinned
        // by NakNotPendingDebugTest.
        char baseBufN2[8];
        Log::log().debug(TAG,
                         "wire NAK missing=%u not pending (base=%s) — ignored",
                         (unsigned)missingCobsSeq,
                         arq_.gbnBaseStrForLog(baseBufN2, sizeof(baseBufN2)));
        return false;
    }
    lastNakSeq_ = missingCobsSeq;
    rxBytes += RX_NAK_WIRE_BYTES;

    // SYNC never populates the ArqCache, so a NAK-driven lookup would
    // miss and resend a zero-byte frame the peer reads as a seq
    // advance. sendMsg's blocking retx ladder owns SYNC recovery.
    //
    // Ownership stays with the ladder; all this does is stop the
    // ladder sleeping through news it already has. The peer NAKs
    // ~6 ms after each retx, yet the ladder sat out the whole
    // syncAckTimeoutMs before resending — four dead RTOs, then a
    // BREAK storm and a full P1 walk to the slowest baud. onNaked
    // is deliberately NOT called here: it only reseats sentAtMs_,
    // which nothing on the SYNC path reads (waitForAck runs its
    // own deadline), so it would be pure state churn.
    if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
        arq_.noteNakWake(missingCobsSeq);
        return false;
    }

    // Cumulative-NAK free (defect 4 in the field
    // field-log analysis): in ASYNC's in-order delivery
    // the receiver only emits a NAK for the
    // reorderExpectedSeq() — every NAK(N) is exactly a
    // cumulative ACK for N-1. The earlier shape
    // never advanced gbnBase on a NAK, so a NAK for
    // seq 30 with base=22 left 22..29 still pending
    // through 3 retx rounds until the watchdog reset
    // and wiped 31 chunks. Advance the base to
    // missingCobsSeq under the same wrap-aliasing
    // guard onAck uses (idxOf + window-bounded walk),
    // so a stale NAK from a prior seq-lap cannot free
    // the live window. The subsequent resend re-emits
    // just missingCobsSeq (the new base) — not the
    // entire prefix, because that prefix is now
    // already-free. Pinned by NakCumulativeFreeTest.
    //
    // was gated off pending a re-validation of
    // the field's NAK semantics on top of the
    // wrap-aliasing guard. lands the corrected
    // walk: per-slot live-window check at every step.
    // idxOf() now uses the 254-value wire space
    // (item 3, see LinkArq.h::idxOf), so a NAK whose
    // missing seq is past the live window (the
    // field-log case) is rejected by the outer idxOf
    // check, and the per-step walk is bounded by the
    // same window. Pinned by NakCumulativeFreeTest.
    //
    // The walk lands the base ON missingCobsSeq
    // (not missing-1) so the resend block below
    // matches on `missingCobsSeq == arq_.gbnBase()`
    // and the inline resend fires. The pre-shape
    // walked to missing-1, leaving missing pending
    // and skipping the resend block — recovery fell
    // entirely to RTO and broke the undamped-inline-
    // NAK contract. Pinned by
    // NakCumulativeFreeBaseLandsOnMissingTest.
    if (arq_.gbnActive() && missingCobsSeq != arq_.gbnBase() &&
        arq_.idxOf(missingCobsSeq) >= 0) {
        int cumulativeFreed = 0;
        while (arq_.gbnBase() != missingCobsSeq) {
            // D9: per-slot live-window check. A NAK
            // whose missingCobsSeq is exactly at the
            // live-window edge can land gbnBase_ on a
            // seq that's already been freed by a
            // prior-lap re-ACK; idxOf() rejecting the
            // step aborts the walk without corrupting
            // the base.
            if (arq_.idxOf(arq_.gbnBase()) < 0 ||
                ++cumulativeFreed > (int)AUTOLINK_ARQ_PIPELINE_WINDOW) {
                Log::log().warning(TAG,
                                   "wire NAK cumulative free exceeded "
                                   "window (base=%u missing=%u) — aborting, "
                                   "invariant violated",
                                   (unsigned)arq_.gbnBase(),
                                   (unsigned)missingCobsSeq);
                break;
            }
            arq_.onAcked(arq_.gbnBase(), 0);
            arqCache_.freeBySeq(arq_.gbnBase(),
                                IArqCache::FreeCause::NakCumulative);
            if (lastNakSeq_ == arq_.gbnBase())
                lastNakSeq_ = 0xFF;
            arq_.setGbnBase(arq_.gbnBase() == COBS_SEQ_MAX
                                ? 0
                                : (uint8_t)(arq_.gbnBase() + 1));
        }
        Log::log().debug(
            TAG,
            "GBN base advanced by NAK: now base=%u (freed %d pre-NAK slots, "
            "missing=%u)",
            (unsigned)arq_.gbnBase(), cumulativeFreed,
            (unsigned)missingCobsSeq);
    }

    // Under in-order delivery only the base can legitimately be
    // missing. A NAK for anything else is stale wire noise.
    //
    // Same-event dedup (defect 2 in the field-log
    // analysis): a NAK describing the same base as the
    // previous resend, inside the resend's flight window,
    // is the same loss event — the peer is reacting to the
    // resend we just sent, not reporting a new loss.
    // Suppress the second resend; the first one is the
    // immediate, undamped full-window blast that
    // loopback_multichunk_test pins. The first NAK after
    // every resend (or after a base advance) still gets an
    // undamped resend — causality, not a per-RTO cap.
    // Pinned by GbnResendSameEventDedupeTest.
    if (arq_.gbnActive() && missingCobsSeq == arq_.gbnBase()) {
        // AL90-9: evaluate the
        // suppression checks BEFORE bumping
        // the ARQ RTO clock. The previous
        // placement reseated sentAtMs_ via
        // onNaked() above all suppression
        // logic, so every incoming NAK
        // pushed the RTO forward — even
        // NAKs we then suppressed as
        // "base stuck" or "same-event
        // dedup". The RTO ladder is the
        // only recovery path for a peer
        // that NAKs without ever ACKing,
        // and the RTO was being deferred
        // by every suppressed NAK on the
        // way through. Pinned by
        // NAKSuppressedDoesNotReseatRtoTest.
        //
        // AL89-6: dedup window now floored at
        // 2×gbnResendFlightMs, and the resend is
        // suppressed entirely when nakCountFor
        // climbs past cfg.maxRetx without the
        // base advancing. The field capture's
        // gbnResendFlightMs collapsed to 8 ms at
        // 512000 baud (2 ms tx time × 2 + 2 ms
        // floor) while the peer's NAK cadence
        // was 16 ms; every NAK fired a fresh
        // full-window resend and the same base
        // accumulated 29 NAKs without
        // advancing — the peer was blocked, not
        // lossy, and a resend cannot help a
        // blocked peer. The 2× floor guarantees
        // the dedup window can never be shorter
        // than one full NAK cadence; the
        // maxRetx gate guarantees a peer that
        // hasn't acknowledged the same base
        // across N round trips gets no
        // additional amplification. Pinned by
        // ResendDedupeFloorAndPeerBlockedTest.
        uint32_t now = hw.nowMs();
        int flightMs = gbnResendFlightMs_unlocked();
        int dedupWindowMs = flightMs * 2;
        if (dedupWindowMs < 16)
            dedupWindowMs = 16;
        uint8_t nakCount = arq_.nakCountFor(missingCobsSeq);
        uint8_t curBase = arq_.gbnBase();
        // "Base hasn't advanced" is
        // approximated as "the seq is the
        // current base" — if missingCobsSeq
        // == curBase, the base is stuck on
        // this seq, and a resend cannot help.
        // For any other seq, the base has
        // either advanced past it (recovered)
        // or never reached it (out-of-order)
        // — the resend is still load-bearing.
        // Without this qualifier the
        // loopback_multichunk test (heavy
        // loss) gets nakCount>maxRetx on
        // the moving base, suppresses every
        // resend, the link stops
        // recovering, and the noise test
        // sees 0 disconnects / 0 baud
        // fallback despite 290 frame
        // errors.
        if (nakCount > (uint8_t)cfg.maxRetx && missingCobsSeq == curBase) {
            Log::log().warning(TAG,
                               "NAK missing=%u suppressed — base stuck, "
                               "nakCount=%u > maxRetx=%u, base=%u",
                               (unsigned)missingCobsSeq, (unsigned)nakCount,
                               (unsigned)cfg.maxRetx, (unsigned)curBase);
            // AL90-9: still bump nakCount_ so a
            // sustained base-stuck eventually
            // trips the watchdog / dropLink,
            // but do NOT touch sentAtMs_ — a
            // suppressed NAK must not defer
            // the RTO ladder.
            arq_.noteSuppressedNak(missingCobsSeq);
            return false;
        }
        if (gbnLastResendBase_ == missingCobsSeq &&
            (uint32_t)(now - gbnLastResendMs_) < (uint32_t)dedupWindowMs) {
            Log::log().debug(TAG,
                             "NAK missing=%u dedup'd — same-event "
                             "(%lu ms < dedup %d ms, flight %d ms)",
                             (unsigned)missingCobsSeq,
                             (unsigned long)(now - gbnLastResendMs_),
                             dedupWindowMs, flightMs);
            // AL92-7: this NAK is suppressed
            // (dedup'd) exactly like the
            // base-stuck case above and must
            // bump nakCount_ the same way —
            // otherwise most NAKs in a storm
            // land here (inside the dedup
            // window) and the base-stuck gate
            // above climbs far slower than a
            // real stuck-base storm warrants.
            arq_.noteSuppressedNak(missingCobsSeq);
            return false;
        }
        // D13: this is the NAK-driven resend
        // path. Stamp the source so the
        // gbnResendWindow_unlocked call
        // forwards Nak into arq_.applyRetx,
        // which bumps nakCount_ only (the
        // retxCount_ storm-stuck verdict gate
        // is left alone). Pinned by
        // GbnStuckNakCountGateTest.
        resendSource_ = ResendSource::Nak;
        // AL92-17: THIS NAK is the one we are
        // about to act on — a real resend goes
        // out below. It must reseat sentAtMs_
        // (via onNaked), the same as any other
        // event that puts a fresh frame on the
        // wire. AL90-9's rationale (a suppressed
        // NAK must not defer the RTO ladder)
        // applies to the two branches ABOVE this
        // one, which return without resending;
        // it does not apply here. The previous
        // AL90-9..AL92 shape called
        // onNakOnlyForTest (counter-only) on
        // this accepted path too, which meant no
        // NAK ever reseated the RTO — a slot
        // that was just NAK-resent could hit its
        // own RTO immediately and fire a
        // duplicate sweep retx on top of the NAK
        // resend. Measured on
        // run_app_gap_stop_soak's ASYNC/random
        // cell: 46/31 delivered with the bug,
        // 102/85 with this fix (baseline before
        // the AL90-9..AL92 regression was 85/58).
        // Pinned by NakResendReseatsRtoTest.
        arq_.onNaked(missingCobsSeq, hw.nowMs());
        gbnResendWindow_unlocked(now);
    }

    return false;
}

bool Link::onFrameError() { return err_unlocked(FrameErrCause::CrcFail); }

} // namespace autolink
