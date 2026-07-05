// Source-level regression pin for the
// bytes-recvd facet wire-up. Closes the
// todo item 5 "latent accessor" decision by
// giving Link::bytesRecvdForMessage(baseSeq)
// its first production consumer.
//
// The original closing of Open 5 was the
// "wire <seq> <bytes>" debug line at every
// slot completion in Ping::loop. That log
// was useful in development (it exposed the
// previously-latent accessor) but pulled
// noise into operator-facing logs at high
// ASYNC rates (the log fired once per slot
// drain, every loop). It was removed in
// a prior release — the production consumer that
// matters is the dashboard JSON path, which
// reads `bytesRecvdForMessage(baseSeq)`
// directly. The facade forwarder stays.
//
// Pins:
//   a) include/AutoLink.h exposes
//      bytesRecvdForMessage(uint8_t) with the
//      same signature Link.h declares (the
//      forwarder is a one-liner that reads from
//      link_ when non-null). The production
//      consumer is the dashboard JSON, so the
//      accessor must remain callable from app
//      code.
//   b) the additive "wire %u %u" debug log
//      that Ping::loop emitted at every slot
//      completion is GONE. Two sites existed
//      (the gap-stop drain and the main loop's
//      tail-drain drain); neither is present
//      in any Ping source. A regression that
//      re-adds the per-slot chatter would
//      needlessly pollute the operator log at
//      ASYNC rates.
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

// Pin b — the per-slot "wire %u %u" debug
// log is GONE from Ping::loop. Removed in
// that release because the operator-facing `echo`
// log already prints the slot's local message
// length, and the per-slot wire-recvd chatter
// was redundant noise at ASYNC rates. The
// production consumer for
// `bytesRecvdForMessage` is now exclusively
// the dashboard JSON.
void test_ping_wire_log_removed() {
    std::cout << "\n=== Ping 'wire <seq> <bytes>' per-slot log is removed ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // No occurrence of the wire-recvd log
    // format string anywhere in Ping.h — not
    // in the loop() body, not in helpers, not
    // in dead-code branches. The log was the
    // only consumer of Ping reading
    // bytesRecvdForMessage; with it gone the
    // accessor stays reachable via the AutoLink
    // facade (Pin a) without spawning per-slot
    // chatter.
    assert(src.find("wire %u %u") == std::string::npos);

    // And the body of loop() doesn't dereference
    // bytesRecvdForMessage at all. Drain sites
    // still log the slot's local `echo` line.
    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());
    assert(body.find("bytesRecvdForMessage(") == std::string::npos);

    // Echo log shape preserved — the
    // operator-facing number is the slot's
    // local len (queue_[head_].len), not the
    // wire-ACK sum.
    assert(body.find("echo %u %u %d") != std::string::npos);
    assert(body.find("queue_[head_].len") != std::string::npos);

    std::cout
        << "  PASS (wire-recvd log absent; echo log + slot-len preserved)\n";
}

} // namespace

int main() {
    std::cout << "=== Running BytesRecvdForwardedToPing Tests ===" << std::endl;
    test_autolink_facade_forwards_bytes_recvd_for_message();
    test_ping_wire_log_removed();
    std::cout << "\n=== BytesRecvdForwardedToPing Tests Completed ==="
              << std::endl;
    return 0;
}

#endif
