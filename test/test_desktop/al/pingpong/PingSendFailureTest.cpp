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
//   - Ping exposes a consecSendFail_ counter that
//     triggers dropLink() + clearQueue_() at
//     MAX_SEND_FAIL (5) consecutive send failures.
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
        // Read the 200 chars after the format string —
        // the next args are the three %u values, ending
        // at the call's closing paren.
        std::string tail = body.substr(fmtPos, 400);
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

// Pin 3: consecSendFail_ counter drives dropLink
// + clearQueue at MAX_SEND_FAIL = 5. Pins the
// escalation path.
void test_ping_consec_send_fail_counter() {
    std::cout << "\n=== Pin 3: consecSendFail_ escalates to dropLink + "
                 "clearQueue_"
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // The counter member is declared with default 0.
    assert(src.find("uint32_t consecSendFail_") != std::string::npos);
    // MAX_SEND_FAIL = 5.
    assert(src.find("MAX_SEND_FAIL = 5") != std::string::npos);
    // In the send loop's failure branch, the counter
    // is bumped and at the threshold dropLink is
    // called.
    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());
    assert(body.find("consecSendFail_++") != std::string::npos);
    assert(body.find("consecSendFail_ >= MAX_SEND_FAIL") != std::string::npos);
    assert(body.find("base_.comm_.dropLink()") != std::string::npos);
    // A successful send must reset the counter.
    assert(body.find("consecSendFail_ = 0") != std::string::npos);
    std::cout << "  PASS (consecSendFail_ + MAX_SEND_FAIL = 5 + "
                 "dropLink on threshold)"
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

} // namespace

int main() {
    std::cout << "=== Running Ping/Pong Send-Failure Regression ==="
              << std::endl;
    test_pong_no_payload_echo();
    test_ping_echo_log_format();
    test_ping_consec_send_fail_counter();
    test_ping_msg_size_policy();
    test_ping_gap_stop_resume();
    test_ping_recv_rejected_drains_rx();
    std::cout << "\n=== Ping/Pong Send-Failure Regression Completed ==="
              << std::endl;
    return 0;
}

#endif
