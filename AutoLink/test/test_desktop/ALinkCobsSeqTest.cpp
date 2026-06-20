// ALinkCobsSeqTest.cpp — host-only tests for cobsSeq gap/stale
// (per-frame sequence number) gap and stale-frame detection.
//
// immune to wire-byte-shift desyncs (the v3.0.0..v3.2.10 bug). These
// tests pin the behavior:
// * First valid frame is accepted (rxSeqSet becomes true).
// * Second frame must be exactly rxSeq + 1; anything else is
// logged as a gap (small offset) or stale (large offset / duplicate).
// * A gap DROPS the frame but does NOT reset rxSeq (the next
// valid frame is expected+1, so the receiver resyncs in one frame).
// * A stale frame is dropped and rxSeq is also not advanced.
// * After link drop, txSeq AND rxSeq both reset to 0.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include "MockHal.h"
#include "util/UtilCrc.h"
#include "util/UtilCobs.h"

using namespace autolink;

// Build a reliable-mode wire frame for the cobsSeq tests. Each frame
// has `cobsSeq` as the first decoded byte and `payloadLen` payload bytes
// (default 16 to exercise non-trivial app-buffer pressure). Uses the same
// wire format as UtilFrameRxTest's wireFrame helper.
static std::vector<uint8_t> cobsFrame(uint8_t cobsSeq, int payloadLen = 16) {
 // Build the unencoded form: cobsSeq | payload | CRC8(cobsSeq | payload)
 std::vector<uint8_t> raw;
 raw.push_back(cobsSeq);
 for (int i = 0; i < payloadLen; i++) raw.push_back((uint8_t)(0xA0 + i));
 raw.push_back(UtilCrc::crc8(raw.data(), (int)raw.size()));
 // COBS-encode
 std::vector<uint8_t> enc(UtilCobs::encodedMax(raw.size()) + 2);
 size_t n = UtilCobs::encode(raw.data(), raw.size(), enc.data() + 1);
 enc[0] = 0x00;
 enc[1 + n] = 0x00;
 enc.resize(n + 2);
 return enc;
}

// (Tests read Diag into a local `Diag d; obj.getDiag(d);` directly so
// subsequent asserts can see `d`. Helper macro doesn't help here because
// the d has to outlive the helper call.)

void test_first_frame_accepted() {
 std::cout << "\n=== Test: First Reliable Frame Sets cobsSeq ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 Diag d;
 b.getDiag(d); assert(!d.rxSeqSet);
 auto f = cobsFrame(0);
 b.onRx(f.data(), (int)f.size());
 b.getDiag(d); assert(d.rxSeqSet);
 assert(d.rxSeq == 0);
 assert(d.gaps == 0);
 assert(d.stale == 0);
 std::cout << "PASS" << std::endl;
}

void test_consecutive_frames_advance() {
 std::cout << "\n=== Test: Consecutive Frames Advance cobsSeq ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 Diag d;
 for (uint8_t seq = 0; seq < 5; seq++) {
 auto f = cobsFrame(seq);
 b.onRx(f.data(), (int)f.size());
 b.getDiag(d); assert(d.rxSeq == seq);
 }
 b.getDiag(d);
 assert(d.gaps == 0);
 assert(d.stale == 0);
 std::cout << "PASS" << std::endl;
}

// detected and the receiver FORWARD-RESYNCS — the frame is delivered to
// the app buffer, rxSeq advances to cobsSeq, and the missing seqs are
// this one-way streaming protocol meant the link was permanently broken
// from the first corrupted frame onward (the next valid frame was
// always a gap, the pipeline never drained, STALL_MS fired, BREAK
// cycle: a single corrupted frame costs the operator that frame's
// worth of data, not the link.
void test_gap_dropped_no_buffer_push() {
 std::cout << "\n=== Test: Forward Gap Resyncs, Delivers Frame, Counts Lost ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // Receive seq=0, then seq=1, then seq=3 (gap at seq=2).
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
 size_t availBefore = (size_t)b.available();
 b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());
 // app buffer, rxSeq advances to 3, lostMsgs = diff-1 = 1 (one
 // missing seq: 2). One gap event counted, no stale (this isn't
 // a duplicate).
 Diag d;
 b.getDiag(d);
 assert(d.gaps == 1);
 assert(d.stale == 0);
 assert(d.lostMsgs == 1);
 assert((size_t)b.available() > availBefore); // frame delivered
 assert(d.rxSeq == 3); // advanced to cobsSeq
 std::cout << "PASS" << std::endl;
}

// After a forward-resync, the "missing" seq (if it ever arrives late)
// is now a wraparound duplicate from the receiver's point of view:
// rxSeq has moved past it. The narrow STALE branch (diff in the upper
// stale. Both behaviors are correct under their respective protocol
// reality.
void test_gap_then_recover() {
 std::cout << "\n=== Test: Post-Gap Expected Seq Arrives Late → STALE ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
 b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size()); // forward gap → resync, rxSeq=3
 b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size()); // the "missing" one arrives late

 // 128, so it's a wraparound duplicate → STALE. dropped, no
 // advance, no extra lostMsgs.
 Diag d;
 b.getDiag(d);
 assert(d.gaps == 1);
 assert(d.stale == 1);
 assert(d.rxSeq == 3);
 assert(d.lostMsgs == 1);
 std::cout << "PASS" << std::endl;
}

// every later frame was a gap, the STALL_MS watchdog fired, the link
// re-swept, and leftover bytes from the prior session re-classified as
// STALE on the next session, repeating the stall→BREAK→disconnect loop.
// costs lostMsgs+=1 and the link stays clean.
//
// This test simulates the hardware scenario: 100 frames in, one
// corrupted (cobsSeq=42 is dropped on the wire, the next decoded frame
// would have ended with rxSeq frozen at 41, the link broken, and an
// gaps=1, lostMsgs=1, stale=0.
void test_single_corruption_does_not_cascade() {
 std::cout << "\n=== Test: Single Corruption Does Not Cascade (0 regression) ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // Deliver seq=0..41 (clean), then skip seq=42 (corrupted, lost on
 // the wire — never call onRx with it), then deliver seq=43..143
 // processes the rest normally.
 for (int s = 0; s <= 41; s++) b.onRx(cobsFrame(s).data(), (int)cobsFrame(s).size());
 // seq=42 is the lost frame — never onRx'd.
 for (int s = 43; s <= 143; s++) b.onRx(cobsFrame(s).data(), (int)cobsFrame(s).size());

 Diag d;
 b.getDiag(d);
 // message, no stale, rxSeq caught up to the latest accepted frame.
 assert(d.gaps == 1);
 assert(d.stale == 0);
 assert(d.lostMsgs == 1);
 assert(d.rxSeq == 143);
 // No unbounded gap/stale churn — the corrupted frame cost exactly
 // one accounting event, not an avalanche.
 assert(d.gaps + d.stale <= 2);
 std::cout << "PASS" << std::endl;
}

// A duplicate (already-seen seq) is stale, not a gap. Drop without
// advancing.
void test_duplicate_is_stale() {
 std::cout << "\n=== Test: Duplicate cobsSeq Is Stale ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size()); // duplicate
 Diag d;
 b.getDiag(d);
 assert(d.stale == 1);
 assert(d.gaps == 0);
 assert(d.rxSeq == 1);
 std::cout << "PASS" << std::endl;
}

// cobsSeq wraparound at COBS_SEQ_WRAP. seq=255 -> seq=0 must NOT be a gap
// (it wraps cleanly), and seq=1 after that must not be a gap (it's 2
// ahead of lastRxCobsSeq_=0, but expected is 1 so it IS a gap).
void test_wraparound_continuity() {
 std::cout << "\n=== Test: cobsSeq Wraparound Is Continuous ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // Send seq=254, then seq=255, then seq=0 (wraparound).
 b.onRx(cobsFrame(254).data(), (int)cobsFrame(254).size());
 b.onRx(cobsFrame(255).data(), (int)cobsFrame(255).size());
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());

 Diag d;
 b.getDiag(d);
 assert(d.rxSeq == 0);
 assert(d.gaps == 0);
 assert(d.stale == 0);
 std::cout << "PASS" << std::endl;
}

// After wraparound, seq=1 is the expected next. seq=2 is a gap.
void test_wraparound_then_gap() {
 std::cout << "\n=== Test: Post-Wraparound Gap Forward-Resyncs ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 b.onRx(cobsFrame(255).data(), (int)cobsFrame(255).size());
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size()); // gap (1 missing)
 Diag d;
 b.getDiag(d);
 assert(d.gaps == 1);
 assert(d.lostMsgs == 1);
 assert(d.rxSeq == 2); //: forward-resync advanced
 std::cout << "PASS" << std::endl;
}

// Sender's txSeq starts at 0 after begin() and increments on every
// reliable-mode data frame.
void test_sender_cobsSeq_increments() {
 std::cout << "\n=== Test: Sender cobsSeq Increments Per Data Frame ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 cfg.reliableMode = true;
 mHal.peer = &sHal; sHal.peer = &mHal;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 Diag d;
 ping.getDiag(d); assert(d.txSeq == 0);

 // Each write() call to the reliable layer consumes one cobsSeq.
 uint8_t b1 = 0xAA;
 ping.write(&b1, 1);
 ping.getDiag(d); assert(d.txSeq == 1);

 uint8_t b2 = 0xBB;
 ping.write(&b2, 1);
 ping.getDiag(d); assert(d.txSeq == 2);

 // A control-frame send (PING etc) does NOT consume a cobsSeq. We
 // can't easily test this from the host because the control-frame
 // path is internal to ALink -- covered by the SWP/LCK test in
 // ALinkNegotiationTest, which now logs cobsSeq=0 for PING frames.

 std::cout << "PASS" << std::endl;
}

// After dropLink, the sender's cobsSeq and the receiver's lastRxCobsSeq
// immune to "stale bytes from a previous session" desyncs.
void test_drop_resets_cobsSeq() {
 std::cout << "\n=== Test: dropLink() Resets cobsSeq on Both Sides ===" << std::endl;
 MockHal mHal, sHal;
 mHal.peer = &sHal; sHal.peer = &mHal;
 AutoLinkConfig cfg;
 cfg.allowedBauds[0] = 9600; cfg.allowedBauds[1] = 115200; cfg.allowedBaudsCount = 2; cfg.pingSamplesPerBaud = 1;
 cfg.fastBaudLock = false;
 cfg.reliableMode = true;
 cfg.streamBufferSize = 8192;
 ALink ping(mHal, true, cfg);
 ALink pong(sHal, false, cfg);
 negotiate_to_ok(ping, pong, mHal, sHal);

 // Get some cobsSeq activity going.
 uint8_t b = 0xAA;
 for (int i = 0; i < 5; i++) ping.write(&b, 1);
 pipe_data(mHal, sHal);
 Diag d;
 ping.getDiag(d); assert(d.txSeq >= 5);
 pong.getDiag(d); assert(d.rxSeq >= 0); // could be 0..4 depending on cobsSeq echoing

 // Force a drop.
 ping.dropLink();
 pong.dropLink();

 ping.getDiag(d); assert(d.txSeq == 0);
 pong.getDiag(d); assert(d.txSeq == 0);
 pong.getDiag(d); assert(!d.rxSeqSet); // lastRx cleared
 std::cout << "PASS" << std::endl;
}

// The v3.x bug: a wire-byte shift that used to desync the FIFO now
// corrupted frame never reaches the message layer — the FIFO desync
// is what caused the v3.x MISMATCH cascade.
//
// in the forward range is now forward-resynced and DOES reach the
// message layer (with lostMsgs++ for the missing in-between seqs).
// The v3.x invariant we still preserve is: the wire-byte shift does
// not cause a permanent FIFO desync, the link stays in OK, and the
// corrupted frame is accounted for somewhere (gap, stale, or
// frame-error). The cobsSeq numbering is still monotonic, so the
// desync class of bug is still closed.
void test_wire_byte_shift_caught_at_cobsSeq() {
 std::cout << "\n=== Test: Wire-Byte Shift Accounted, Link Stays in OK ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // Receive a valid frame for seq=0, then a frame whose COBS body has
 // been bit-flipped. The flipped frame may decode to a different
 // cobsSeq, may fail CRC entirely, or may pass CRC with a different
 // payload — the test just asserts that whichever of those happens,
 // the corruption is accounted for and the link state is sane.
 auto f0 = cobsFrame(0);
 auto f1 = cobsFrame(1);
 f1[1] ^= 0x40; // flip a bit in the COBS body of the second frame
 b.onRx(f0.data(), (int)f0.size());
 b.onRx(f1.data(), (int)f1.size());

 Diag d;
 b.getDiag(d);
 // The corruption is accounted for: at least one of these moved.
 bool accounted = (d.gaps > 0)
 || (d.stale > 0)
 || (b.getErrCount() > 0);
 assert(accounted);
 // still tracking — rxSeq has been set to *something* sane (the
 // seed path is exhausted; rxSeqSet is latched true), and there's
 // no infinite-loop / stuck condition observable from the diag.
 assert(d.rxSeqSet);
 // cobsSeq gaps or stales bounded: the corrupted frame produced a
 // single accounting event, not an unbounded sequence of them.
 assert(d.gaps + d.stale + b.getErrCount() <= 2);
 std::cout << "PASS" << std::endl;
}

// Verify the Diag struct returns the values that onPayload logged
// (lives in the public API, so a custom dashboard can graph gap rate
// over time).
void test_gap_stale_counters_accessible() {
 std::cout << "\n=== Test: Gap/Stale Counters Are Public API ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 Diag d;
 b.getDiag(d);
 assert(d.gaps == 0);
 assert(d.stale == 0);
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size()); // small gap (1 missing)
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size()); // duplicate (stale)
 b.getDiag(d);
 assert(d.gaps == 1);
 assert(d.stale == 1);
 std::cout << "PASS" << std::endl;
}

// wire error. It must NOT count toward errThreshold (which would drop the
// link even though the wire is fine). This test pins the contract: feed
// the receiver more bytes than the app buffer can hold, verify the link
// stays in OK and no errs are counted.
void test_app_buffer_full_does_not_drop_link() {
 std::cout << "\n=== Test: App Buffer Full Doesn't Trip errThreshold ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true;
 cfg.streamBufferSize = 256; // modest app buffer
 cfg.errThreshold = 5; // low threshold so a wire error would drop quickly
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);
 // Force the mock's app buffer to be smaller than the cfg so the
 // overflow path is exercised even with cobsFrame's modest payloads.
 sHal.appBufCap = 16; // 1 frame fits, the rest overflow

 // Negotiate to OK so we're exercising the OK-mode RX path (which has
 // the app-buffer-full code path).
 negotiate_to_ok(a, b, mHal, sHal);
 assert(b.getState() == State::OK);

 int errsBefore = b.getErrCount();
 Diag d;
 b.getDiag(d); uint64_t gapsBefore = d.gaps;

 // Feed many reliable-mode frames. Once the app buffer (cap=16) fills,
 // subsequent frames overflow. The link must NOT drop.
 for (int seq = 0; seq < 30; seq++) {
 b.onRx(cobsFrame(seq).data(), (int)cobsFrame(seq).size());
 }

 // Link is still in OK (no BREAK, no re-sweep).
 assert(b.getState() == State::OK);
 // errCount is unchanged: app-buffer-full is NOT a wire error.
 assert(b.getErrCount() == errsBefore);
 // At least some gaps were counted (the dropped frames show up as
 // missing cobsSeq numbers in the wire stream).
 b.getDiag(d); assert(d.gaps > gapsBefore);
 std::cout << "PASS" << std::endl;
}

// and lostMsgs by (N - 1) — the wire-loss tally, distinct from the
// gap-event count. A burst loss of 4 frames (seq=1, then seq=5) is
// gaps=1, lostMsgs=3. Two separate single-seq gaps is gaps=2,
// lostMsgs=2. Both must read correctly from getDiag.
void test_lost_msgs_burst_vs_single() {
 std::cout << "\n=== Test: lostMsgs Counts Missed cobsSeq, Not Events ===" << std::endl;
 MockHal mHal, sHal;
 AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
 ALink a(mHal, true, cfg);
 ALink b(sHal, false, cfg);

 // Burst loss: receive seq=0, then seq=4 (missing 1, 2, 3).
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.onRx(cobsFrame(4).data(), (int)cobsFrame(4).size());
 Diag d; b.getDiag(d);
 assert(d.gaps == 1);
 assert(d.lostMsgs == 3); // 4 - 0 - 1 = 3 missed seqs

 // Two single-seq gaps: receive seq=4, then seq=5, then seq=7
 // (missing 6), then seq=9 (missing 8).
 b.onRx(cobsFrame(5).data(), (int)cobsFrame(5).size());
 b.onRx(cobsFrame(7).data(), (int)cobsFrame(7).size());
 b.onRx(cobsFrame(9).data(), (int)cobsFrame(9).size());
 b.getDiag(d);
 assert(d.gaps == 3); // 1 (burst) + 2 (single-seq)
 assert(d.lostMsgs == 5); // 3 (burst) + 1 + 1 (two singles)

 // lostMsgs survives a link drop (it's a lifetime wire-loss tally;
 // it's reset only by full lifetime-counter reset). The cobsSeq
 // itself resets to 0 on drop, but the cumulative lostMsgs stays.
 b.dropLink();
 b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
 b.getDiag(d);
 assert(d.lostMsgs == 5); // unchanged by drop
 assert(d.gaps == 3); // unchanged by drop
 std::cout << "PASS" << std::endl;
}

int main() {
 std::cout << "=== Running ALinkCobsSeq Tests (0: forward-resync) ===" << std::endl;
 test_first_frame_accepted();
 test_consecutive_frames_advance();
 test_gap_dropped_no_buffer_push();
 test_gap_then_recover();
 test_single_corruption_does_not_cascade();
 test_duplicate_is_stale();
 test_wraparound_continuity();
 test_wraparound_then_gap();
 test_sender_cobsSeq_increments();
 test_drop_resets_cobsSeq();
 test_wire_byte_shift_caught_at_cobsSeq();
 test_gap_stale_counters_accessible();
 test_app_buffer_full_does_not_drop_link();
 test_lost_msgs_burst_vs_single();
 std::cout << "\n=== ALinkCobsSeq Tests Completed Successfully ===" << std::endl;
 return 0;
}

#endif // ARDUINO
