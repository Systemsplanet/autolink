// Source-level + structural regression test for the
// this-release Ping/Pong role changes:
//
//   - Pong does NOT echo the payload back. Pong's
//     loop is recv-and-count (ackCount_). The wire-
//     level ACK is the entire Pong-side response.
//   - Pong's diagnostic log line is
//     "echo <seq> <bytes>" (crc=ok implicit).
//   - Ping's diagnostic log line on a slot
//     completion is
//     "echo <seq> <bytesAcked> <pending>".
//   - Ping's mismatch drain path (got<0) still
//     calls clearQueue_() + flushRx().
//   - Ping's send-failure branch must NOT escalate
//     to dropLink on backpressure retries; the
//     ARQ cache saturation is backpressure, not a
//     dead-peer signal, and the counter-then-drop
//     shape that pre-dated this release turned a
//     saturated ASYNC flood into a resweep loop
//     (disc climbs 1→N on both peers, link never
//     recovers). The cooldown stamp on a saturated
//     cache is the throttle; the link's own
//     resweep is the recovery loop.
//   - Ping's gap-stop / gap-resume signal reads
//     Link::lastNakSeq() / lastAckSeq() and pauses
//     the send loop while gapSeq_ != NO_GAP.
//
// These are mostly source-grep pins because Ping
// and Pong are `#ifdef ARDUINO` and can't run on
// host. The wire-ACK-extension side is pinned by
// LinkFrameRxTest's runtime test (ACK 5-byte
// shape end-to-end).
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

std::string extractFnBody(const std::string &src, const std::string &name) {
    auto start = src.find(name);
    if (start == std::string::npos)
        return "";
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(start, i + 1 - start);
        }
    }
    return "";
}

// Pin 1: Pong does NOT echo the payload back. The
// recv-driven loop only counts and logs the wire
// ACK; it must not call base_.comm_.send(...) with
// the received bytes.
void test_pong_no_payload_echo() {
    std::cout << "\n=== Pin 1: Pong loop has no payload-echo send ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // The recv-driven loop must call base_.comm_.recv
    // but must NOT call base_.comm_.send on the
    // payload bytes (the wire-level ACK is the
    // entire Pong-side response).
    assert(body.find("base_.comm_.recv(") != std::string::npos);
    // No payload-echo send.
    assert(body.find("base_.comm_.send(base_.buf_") == std::string::npos);
    // The diagnostic log line uses the new format
    // "echo <seq> <bytes>" (crc=ok implicit because
    // the link layer verified the per-chunk CRC
    // before reaching onPayload).
    assert(body.find("echo %u %d") != std::string::npos);
    // ackCount_ is the running tally of acks sent.
    assert(body.find("ackCount_++") != std::string::npos);
    // The /mode/toggle-style "SEND FAILED (link
    // dropped)" prose is gone (no send path).
    assert(src.find("SEND FAILED (link dropped)") == std::string::npos);
    assert(src.find("send skipped (link not ready)") == std::string::npos);
    std::cout << "  PASS (Pong recv-only; 'echo <seq> <bytes>' diagnostic; "
                 "no payload-echo send)"
              << std::endl;
}

// Pin 2: Ping's slot-completion log line uses the
// new format "echo <seq> <bytes> <pending>". The
// bytes value is the message size from the local
// slot (queue_[head_].len), NOT the per-frame ACK's
// bytes-recvd — the ACK reports the merged-chunk
// length the peer pushed into its app buffer
// (6-byte MSG_HDR + first chunk payload for a
// multi-chunk message), which is the chunk length,
// not the user-visible message size.
void test_ping_echo_log_format() {
    std::cout
        << "\n=== Pin 2: Ping 'echo <seq> <msgBytes> <pending>' log format"
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // The format string must be the 3-arg echo.
    assert(src.find("echo %u %u %d") != std::string::npos);
    // The bytes value is queue_[head_].len, NOT
    // bytesRecvdFor() — that was the bug. Pin both
    // that the slot's len is read and that
    // bytesRecvdFor() is NOT used in the echo log
    // path. The link layer still exposes
    // bytesRecvdFor() for other consumers; this pin
    // is scoped to the echo log line's bytes source.
    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());
    assert(body.find("queue_[head_].len") != std::string::npos);
    // Extract the slot-completion branches (each
    // calls base_.log_.debug("Ping", "echo %u %u %d",
    // ...)). The bytes arg must be queue_[head_].len
    // in both, and bytesRecvdFor(...) must NOT
    // appear as the bytes arg in either. We do this
    // by checking the substring between the echo
    // format string and the head_++ advance.
    size_t searchFrom = 0;
    int echoSites = 0;
    int lenSites = 0;
    while (true) {
        auto fmtPos = body.find("echo %u %u %d", searchFrom);
        if (fmtPos == std::string::npos)
            break;
        echoSites++;
        // Read the next 1500 chars after the format
        // string — covers the 3-arg echo call, any
        // additive log lines (e.g., the wire-recvd
        // log added by the bytes-recvd facet), and
        // the head_++ slot advance. The pin's intent
        // is "bytes arg is queue_[head_].len,
        // not bytesRecvdFor()", so the window just
        // needs to reach the head advance; 1500
        // covers the current layout with margin.
        std::string tail = body.substr(fmtPos, 1500);
        auto headAdvance = tail.find("head_ = (head_ + 1)");
        assert(headAdvance != std::string::npos);
        std::string echoCall = tail.substr(0, headAdvance);
        // The bytes arg (second %u) must be the local
        // slot's len, not a bytesRecvdFor() lookup.
        assert(echoCall.find("queue_[head_].len") != std::string::npos);
        // No bytesRecvdFor() call inside the echo log
        // call (the bytes must come from local state).
        assert(echoCall.find("bytesRecvdFor(") == std::string::npos);
        lenSites++;
        searchFrom = fmtPos + 1;
    }
    // Two echo sites: the gap-stop branch and the
    // main loop's tail queue drain. Both must use
    // the local slot's len.
    assert(echoSites == 2);
    assert(lenSites == 2);
    std::cout << "  PASS (2 echo sites; both read queue_[head_].len; "
                 "no bytesRecvdFor() in the echo log path)"
              << std::endl;
}

// Pin 3: backpressure retries must NOT escalate
// to dropLink. The pre-this-pin shape bumped a
// consecSendFail_ counter on every backpressure
// send-failure and called dropLink() at
// MAX_SEND_FAIL = 5 — under a legitimate ASYNC
// flood, the ARQ cache saturates, sendMsg returns
// false on every iteration, and the counter hits
// the threshold within BACKPRESSURE_COOLDOWN_MS.
// The drop forces a resweep; the relock re-floods;
// the cache saturates again; the counter hits the
// threshold again; disc climbs 1→N on both peers;
// the link never recovers. Backpressure is the
// link's own self-throttle — Ping's job is to pace
// sends (cooldown, not drop) and the link layer's
// ARQ retransmit loop drains the cache.
void test_ping_backpressure_does_not_drop_link() {
    std::cout << "\n=== Pin 3: backpressure retries do NOT escalate to "
                 "dropLink ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Locate the sendMsg-failure branch.
    auto sendFailPos = body.find("!base_.comm_.sendMsg(sendBuf_");
    assert(sendFailPos != std::string::npos);
    auto branchStart = body.find('{', sendFailPos);
    assert(branchStart != std::string::npos);
    int depth = 0;
    size_t branchEnd = body.size();
    for (size_t i = branchStart; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                branchEnd = i + 1;
                break;
            }
        }
    }
    std::string branch = body.substr(sendFailPos, branchEnd - sendFailPos);

    // No consecSendFail_++ bump in the backpressure
    // branch — backpressure retries are not a peer-
    // death signal.
    assert(branch.find("consecSendFail_++") == std::string::npos);
    // No MAX_SEND_FAIL threshold check.
    assert(branch.find("consecSendFail_ >= MAX_SEND_FAIL") ==
           std::string::npos);
    // No dropLink() call. The link is its own
    // recovery loop; Ping's job is to pace sends.
    assert(branch.find("base_.comm_.dropLink()") == std::string::npos);
    // The cooldown stamp must still be set so the
    // next send waits BACKPRESSURE_COOLDOWN_MS.
    assert(branch.find("backpressureCoolUntilMs_") != std::string::npos);
    // The diagnostic line must report arqPendingCount()
    // (the actual saturated cache), NOT count_ (Ping's
    // local echo queue, which reads 0 when the cache is
    // what saturated).
    assert(branch.find("arqPendingCount") != std::string::npos);
    assert(branch.find("pending=%d") == std::string::npos);
    std::cout << "  PASS (backpressure retries log + cooldown; no "
                 "consecSendFail_ bump; no dropLink; arqPendingCount in log)"
              << std::endl;
}

// Pin 4: Sequential mode grows the message size
// from 1 byte up to maxSeqSize_ (== base.comm.maxMsg())
// and wraps back to 1. Random mode picks a random
// size in [RANDOM_MIN_BYTES=1, maxSeqSize_]. The
// pre-fix values (RANDOM_MIN_BYTES=1024 and a
// hard-coded maxSeqSize_=1024) collapsed the random
// range to a single value (always 1024) and capped
// sequential at 1024 regardless of cfg.maxMsg.
void test_ping_msg_size_policy() {
    std::cout << "\n=== Pin 4: Ping msg-size policy (sequential + random) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // The size-picker is a private method pickMsgSize_
    // that branches on FillMode.
    assert(src.find("int pickMsgSize_(FillMode m)") != std::string::npos);
    // RANDOM_MIN_BYTES = 1 (1-byte floor; the
    // pre-fix 1024 collapsed the range to one value).
    assert(src.find("RANDOM_MIN_BYTES = 1") != std::string::npos);
    // maxSeqSize_ is initialized from
    // base_.comm_.maxMsg() in setup(), not hard-coded.
    std::string setupBody = extractFnBody(src, "void setup()");
    assert(!setupBody.empty());
    assert(setupBody.find("maxSeqSize_ = (int)base_.comm_.maxMsg()") !=
           std::string::npos);
    // Sequential advances seqSize_ 1 -> 2 -> ... ->
    // maxSeqSize_ -> 1 (wraps). The increment is in
    // the send-loop after a successful send.
    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());
    assert(body.find("seqSize_++") != std::string::npos);
    assert(body.find("seqSize_ > maxSeqSize_") != std::string::npos);
    std::cout << "  PASS (sequential 1..maxSeqSize_=comm.maxMsg(); "
                 "random 1..maxSeqSize_)"
              << std::endl;
}

// Pin 5: ASYNC gap-stop. Ping pauses its send loop
// while gapSeq_ != NO_GAP. Resumes when the gap seq
// is ACKed. The signal is Link::lastNakSeq() /
// lastAckSeq().
void test_ping_gap_stop_resume() {
    std::cout << "\n=== Pin 5: Ping gap-stop on peer-detected gap + "
                 "gap-resume on ACK"
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // gapSeq_ is the per-slot signal; default NO_GAP.
    assert(src.find("static constexpr uint8_t NO_GAP = 0xFF") !=
           std::string::npos);
    assert(src.find("uint8_t gapSeq_ = NO_GAP") != std::string::npos);
    // The gap-stop / gap-resume branch reads
    // lastNakSeq() + lastAckSeq() and updates
    // gapSeq_.
    assert(body.find("base_.comm_.lastNakSeq()") != std::string::npos);
    assert(body.find("base_.comm_.lastAckSeq()") != std::string::npos);
    // Pause log line.
    assert(body.find("gap stop: missing seq=") != std::string::npos);
    assert(body.find("gap resumed: seq=") != std::string::npos);
    std::cout << "  PASS (gapSeq_ + lastNakSeq()/lastAckSeq() + "
                 "pause/resume logs)"
              << std::endl;
}

// Pin 6: pong-mismatch drain. The recv-rejected
// branch (got<0) still calls clearQueue_() +
// flushRx() (matches the 5.3.x log-hygiene pin,
// kept here to make sure the recv-only Pong
// migration didn't accidentally drop the rx
// drain).
void test_ping_recv_rejected_drains_rx() {
    std::cout << "\n=== Pin 6: got<0 branch still drains rx via flushRx after "
                 "clearQueue_"
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    auto gotPos = body.find("if (got < 0)");
    assert(gotPos != std::string::npos);
    auto branchStart = body.find('{', gotPos);
    assert(branchStart != std::string::npos);
    int depth = 0;
    size_t branchEnd = body.size();
    for (size_t i = branchStart; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                branchEnd = i + 1;
                break;
            }
        }
    }
    std::string gotBranch = body.substr(gotPos, branchEnd - gotPos);
    auto clearPos = gotBranch.find("clearQueue_()");
    auto flushPos = gotBranch.find("base_.comm_.flushRx()");
    assert(clearPos != std::string::npos);
    assert(flushPos != std::string::npos);
    assert(flushPos > clearPos);
    std::cout << "  PASS (got<0 + clearQueue_ + flushRx in order)" << std::endl;
}

// Pin 7: send-failure branch's link-not-OK guard.
// When sendMsg returns false because the link is
// not OK (state == SWP/LCK), the link is already
// self-recovering via the sweep; Ping must NOT
// log/cooldown in that case (the wire is busy
// doing sweep work, not waiting on Ping's pacing).
// The guard: `if (!base_.comm_.ready()) { break; }`
// must come BEFORE any log/cooldown stamp so the
// link-not-OK path is a true early-return with
// no side effects.
void test_ping_send_fail_guards_on_link_not_ok() {
    std::cout << "\n=== Pin 7: send-failure branch guards on link-not-OK "
                 "(early-return, no log/cooldown) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Locate the sendMsg-failure branch.
    auto sendFailPos = body.find("!base_.comm_.sendMsg(sendBuf_");
    assert(sendFailPos != std::string::npos);
    auto branchStart = body.find('{', sendFailPos);
    assert(branchStart != std::string::npos);
    int depth = 0;
    size_t branchEnd = body.size();
    for (size_t i = branchStart; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                branchEnd = i + 1;
                break;
            }
        }
    }
    std::string branch = body.substr(sendFailPos, branchEnd - sendFailPos);

    // The ready() guard must be present and must
    // early-return BEFORE any backpressure log or
    // cooldown stamp — the link is self-recovering,
    // Ping must not pile on.
    auto readyPos = branch.find("!base_.comm_.ready()");
    auto logPos = branch.find("send failed (backpressure)");
    auto coolPos = branch.find("backpressureCoolUntilMs_ = millis()");
    assert(readyPos != std::string::npos &&
           "send-failure branch must check !base_.comm_.ready()");
    assert(readyPos < logPos &&
           "the ready() check must come BEFORE the backpressure "
           "log line so link-not-OK is a true early-return");
    assert(readyPos < coolPos &&
           "the ready() check must come BEFORE the cooldown stamp "
           "so link-not-OK does not throttle the next iteration");

    std::cout << "  PASS (link-not-OK guard precedes backpressure log + "
                 "cooldown)"
              << std::endl;
}

// Pin 8: Ping flushes the raw rx path on the
// SWP→OK transition (wasReady_ → true branch).
// A half-received frame from before the drop
// (header parsed but payload truncated by the
// link reset) would land at the next recvMsg
// and fail the CRC. flushRx() clears the rx
// stream buffer AND the hw-level queue so the
// first post-recovery message starts clean.
// Mirrors Pong's pre-blink drain (which already
// had a drain loop but lacked the hardware
// flush).
void test_ping_flushrx_at_swp_to_ok() {
    std::cout << "\n=== Pin 8: Ping flushes raw rx on SWP→OK transition ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // Locate the !wasReady_ branch in Ping::loop
    // (the SWP→OK transition handling).
    auto wasReadyPos = src.find("if (!base_.wasReady_)");
    assert(wasReadyPos != std::string::npos);
    auto branchStart = src.find('{', wasReadyPos);
    assert(branchStart != std::string::npos);
    int depth = 0;
    size_t branchEnd = src.size();
    for (size_t i = branchStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                branchEnd = i + 1;
                break;
            }
        }
    }
    std::string branch = src.substr(wasReadyPos, branchEnd - wasReadyPos);

    // The fix shape: base_.comm_.flushRx() must
    // appear before the recv() drain loop in the
    // !wasReady_ block. Pre-fix Ping only drained
    // via recv(), which leaves a half-received
    // frame in the rx buffer to fail the next
    // CRC.
    auto flushPos = branch.find("base_.comm_.flushRx()");
    auto recvPos = branch.find("base_.comm_.recv(recvBuf_");
    assert(flushPos != std::string::npos &&
           "!wasReady_ block must call base_.comm_.flushRx()");
    assert(recvPos != std::string::npos &&
           "!wasReady_ block must still drain via recv()");
    assert(flushPos < recvPos &&
           "flushRx() must come BEFORE the recv() drain so a "
           "half-received frame can't poison the next CRC");

    std::cout << "  PASS (flushRx() before recv() drain in !wasReady_ "
                 "block)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Ping/Pong Send-Failure Regression ==="
              << std::endl;
    test_pong_no_payload_echo();
    test_ping_echo_log_format();
    test_ping_backpressure_does_not_drop_link();
    test_ping_msg_size_policy();
    test_ping_gap_stop_resume();
    test_ping_recv_rejected_drains_rx();
    test_ping_send_fail_guards_on_link_not_ok();
    test_ping_flushrx_at_swp_to_ok();
    std::cout << "\n=== Ping/Pong Send-Failure Regression Completed ==="
              << std::endl;
    return 0;
}

#endif
