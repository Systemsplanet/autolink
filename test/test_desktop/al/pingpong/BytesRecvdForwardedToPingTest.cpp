// Source-level regression pin for the
// bytes-recvd facet wire-up. Closes the
// todo item 5 "latent accessor" decision by
// giving Link::bytesRecvdForMessage(baseSeq)
// its first production consumer.
// into:
//   1. the AutoLink facade forwarder, so the
//      accessor is callable from app code and
//      dashboard JSON;
//   2. Ping's slot-completion branch, where a
//      new "wire <seq> <bytes>" debug line
//      reports the wire-ACK bytes-recvd sum on
//      every successful ack — giving the
//      previously-latent Link::bytesRecvdForMessage
//      its first production consumer and closing
//      Open 5 from todo.md.
//
// Pins:
//   a) include/AutoLink.h exposes
//      bytesRecvdForMessage(uint8_t) with the
//      same signature Link.h declares (the
//      forwarder is a one-liner that reads from
//      link_ when non-null); without it the
//      facade can't expose the accessor.
//   b) Ping's "wire %u %u" log format string
//      appears in TWO echo-completion sites
//      (the gap-stop drain and the main loop's
//      tail-drain drain in Ping::loop). The
//      wire-recvd is logged alongside the
//      existing "echo %u %u %d" line, not
//      instead of it — the slot-len log is the
//      operator-facing number, the wire-recvd
//      is the peer-confirmed sum.
//   c) the wire-recvd log call's bytes arg
//      reads from base_.comm_.bytesRecvdForMessage
//      (facade forwarder), NOT from
//      link->bytesRecvdForMessage directly (Ping
//      doesn't have a `link_` member — it talks
//      to the comm_ facade member). Reading
//      through the facade keeps the dashboard
//      JSON path and the Ping path on the same
//      surface.
//   d) the wire-recvd log call sits INSIDE the
//      isAcked-driven drain of a slot completion,
//      before the head_ / count_ advance — so
//      every successful ack emits one "wire"
//      line per slot, matching the "echo" rate.
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

// Pin a — AutoLink facade forwarder present.
void test_autolink_facade_forwards_bytes_recvd_for_message() {
    std::cout
        << "\n=== AutoLink facade exposes bytesRecvdForMessage(baseSeq) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/include/AutoLink.h");
    assert(!src.empty());

    // The forwarder must exist on the AutoLink
    // class. Match the signature exactly:
    //     uint16_t bytesRecvdForMessage(uint8_t baseSeq) const
    // (the const qualification and uint8_t type
    // are the contract). A forwarder that takes
    // an int / drops the const / is named
    // differently trips this pin.
    assert(src.find("uint16_t bytesRecvdForMessage(uint8_t baseSeq) const") !=
           std::string::npos);

    // The forwarder body must delegate to the
    // link's bytesRecvdForMessage (the protected
    // Link accessor that walks the baseSeq_
    // table). A body that returns a constant,
    // or reads a different field, leaves the
    // accessor latent again.
    std::string fwdBody = extractFnBody(
        src, "uint16_t bytesRecvdForMessage(uint8_t baseSeq) const");
    assert(!fwdBody.empty());
    assert(fwdBody.find("link->bytesRecvdForMessage(baseSeq)") !=
           std::string::npos);

    std::cout
        << "  PASS (facade forwarder delegates to link->bytesRecvdForMessage)\n";
}

// Pin b — Ping has two "wire %u %u" log sites
// in Ping::loop, one per slot-completion drain.
void test_ping_wire_log_format_in_two_sites() {
    std::cout
        << "\n=== Ping emits 'wire <seq> <bytes>' on every slot completion ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // There are exactly two slot-completion
    // drains in Ping::loop: the gap-stop drain
    // (top of the ack-receive path) and the
    // main loop's tail-drain. Both must emit
    // the wire-recvd log alongside the existing
    // echo log. Source-grep the body for both
    // sites.
    auto firstWire = body.find("wire %u %u");
    assert(firstWire != std::string::npos);
    auto secondWire = body.find("wire %u %u", firstWire + 1);
    assert(secondWire != std::string::npos);
    // And there are exactly two sites (no
    // accidental third copy).
    auto thirdWire = body.find("wire %u %u", secondWire + 1);
    assert(thirdWire == std::string::npos);

    // The "wire" log is additive — the existing
    // "echo %u %u %d" log still fires at both
    // sites. The Pin 2 source-grep test in
    // PingSendFailureTest already enforces that
    // the echo log shape is preserved; here we
    // just sanity-check the wire log doesn't
    // replace the echo log.
    auto firstEcho = body.find("echo %u %u %d");
    assert(firstEcho != std::string::npos);
    auto secondEcho = body.find("echo %u %u %d", firstEcho + 1);
    assert(secondEcho != std::string::npos);
    std::cout << "  PASS (2 wire-recvd sites; echo sites preserved)\n";
}

// Pin c — the wire-recvd log reads from the
// facade forwarder, not from a link_ member
// (Ping composes an AutoLink facade as
// `comm_`, not a Link).
void test_ping_wire_log_reads_facade_accessor() {
    std::cout
        << "\n=== Ping 'wire' log reads base_.comm_.bytesRecvdForMessage ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // Each "wire %u %u" call's args block
    // must reference
    // base_.comm_.bytesRecvdForMessage, NOT
    // bytesRecvdFor (single-chunk, the bug
    // shape from pre-fix code) and NOT
    // bytesRecvdForMessage on a different
    // object (e.g. link_ or arq_). We walk
    // the body from the start of each
    // "wire %u %u" call until the matching
    // closing `);` and inspect the call.
    //
    // The source may split `base_.comm_` and
    // `.bytesRecvdForMessage` across lines +
    // whitespace (the clang-formatter puts the
    // call on a continuation indent); strip
    // whitespace before the match so a "wired
    // to a different method" revert (e.g.,
    // tail.find("bytesRecvdFor(") matching
    // bytesRecvdForMessage()) still trips this
    // pin.
    size_t searchFrom = 0;
    int sites = 0;
    while (true) {
        auto fmtPos = body.find("\"wire %u %u\"", searchFrom);
        if (fmtPos == std::string::npos)
            break;
        sites++;
        // 400 chars after the format string
        // covers the full chained call
        // (`base_.comm_.bytesRecvdForMessage(queue_[head_].seq)`)
        // even with multi-line continuations.
        std::string rawTail = body.substr(fmtPos, 400);
        std::string tail;
        tail.reserve(rawTail.size());
        for (char c : rawTail) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            tail.push_back(c);
        }
        // The accessor must be
        // bytesRecvdForMessage — the per-MESSAGE
        // sum across all chunks sharing baseSeq.
        // bytesRecvdFor is the single-chunk accessor
        // and would silently log only the first
        // chunk's bytes for multi-chunk ASYNC.
        // Whitespace-stripped match:
        // `base_.comm_.bytesRecvdForMessage(`.
        assert(tail.find("base_.comm_.bytesRecvdForMessage(") !=
               std::string::npos);
        // bytesRecvdFor (single-chunk) must NOT
        // appear in the args block as a method
        // call (vs. appearing as the prefix of
        // bytesRecvdForMessage). Whitespace-stripped
        // match: `base_.comm_.bytesRecvdFor(` NOT
        // followed by `M`. We test that the
        // stripped form `bytesRecvdFor(` is
        // present only as the Message-call's
        // prefix — which we asserted above.
        // Assert the bare "bytesRecvdFor(" (without
        // trailing "M") is NOT in the tail.
        auto end = tail.find("base_.comm_.bytesRecvdForMessage(");
        // Walk back from the match to confirm
        // there's no bytesRecvdFor( that's NOT
        // followed by M. We search for
        // `.bytesRecvdFor(` and check the next
        // char.
        auto bareFn = tail.find(".bytesRecvdFor(");
        if (bareFn != std::string::npos) {
            // Could be the Message() call's
            // prefix — that's fine. After `M` it's
            // NOT a bare single-chunk call.
            char after = tail[bareFn + std::string(".bytesRecvdFor(").size()];
            assert(after == 'M');
        }
        // Silence unused-variable warning for `end`
        // when asserts are disabled.
        (void)end;
        searchFrom = fmtPos + 1;
    }
    // Two sites (matches Pin b).
    assert(sites == 2);
    std::cout
        << "  PASS (2 sites; each reads base_.comm_.bytesRecvdForMessage, "
           "no single-chunk bytesRecvdFor in the wire log path)\n";
}

// Pin d — the wire-recvd log call sits
// inside the isAcked-driven drain, before
// the head_ / count_ advance. The log
// must fire for every slot completion
// (matching the echo log rate), not just
// once per ack batch.
void test_ping_wire_log_fires_per_slot() {
    std::cout << "\n=== Ping 'wire' log fires inside the slot-drain loop ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());

    // For each "wire %u %u" call site, the
    // next 200-char block must contain
    // `head_ = (head_ + 1)` (the slot advance)
    // BEFORE any closing of the inner
    // `while (count_ > 0 && base_.comm_.isAcked(...))`
    // — i.e., the log fires inside the loop,
    // not after it.
    size_t searchFrom = 0;
    int sites = 0;
    while (true) {
        auto fmtPos = body.find("\"wire %u %u\"", searchFrom);
        if (fmtPos == std::string::npos)
            break;
        sites++;
        std::string tail = body.substr(fmtPos, 400);
        auto headAdvance = tail.find("head_ = (head_ + 1)");
        assert(headAdvance != std::string::npos);
        // The wire log must be the LAST log
        // call before the head advance — i.e.,
        // no `count_--` advance before the log.
        // We can't search backwards easily without
        // a parser, but we can sanity-check that
        // the head advance is present at all and
        // that no second head-advance appears in
        // the tail (which would mean the wire
        // log is before or after a slot advance
        // it shouldn't be).
        auto secondAdvance = tail.find("head_ = (head_ + 1)", headAdvance + 1);
        assert(secondAdvance == std::string::npos);
        searchFrom = fmtPos + 1;
    }
    assert(sites == 2);
    std::cout << "  PASS (2 sites; each inside the per-slot drain loop)\n";
}

} // namespace

int main() {
    std::cout << "=== Running BytesRecvdForwardedToPing Tests ===" << std::endl;
    test_autolink_facade_forwards_bytes_recvd_for_message();
    test_ping_wire_log_format_in_two_sites();
    test_ping_wire_log_reads_facade_accessor();
    test_ping_wire_log_fires_per_slot();
    std::cout << "\n=== BytesRecvdForwardedToPing Tests Completed ==="
              << std::endl;
    return 0;
}

#endif
