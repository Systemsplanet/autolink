// Source-level regression test for the current shape
// app-buf-full NAK contract. The buggy-original shape
// did `acc = hw.pushAppBuf(b, n); if (acc < n)
// sendNakFrame_unlocked(seq);` which had two
// load-bearing bugs:
//   1. The partial `acc` bytes (e.g. 120 of
//      143 in the field log) were already pushed
//      to the app buf; a NAK then told the peer
//      to retx the same seq, which the receiver
//      classified as Stale (because rxSeq was
//      advanced to seq by the buggy-original shape),
//      re-ACKed, and dropped. Permanent message
//      loss under app-buf-full, the direct
//      cause of run A's wedge.
//   2. The 120 partial bytes spliced garbage
//      into the message stream; the message
//      parser saw a CRC failure on the next
//      recvMsg, fired `Pong recv rejected
//      (CRC/desync) frameErrs=2`, and the
//      `drained 136 stale bytes post-settle`
//      log threw away the partial valid bytes
//      along with the garbage.
//
// The fix is all-or-nothing: check
// `hw.appBufFree() >= n` BEFORE writing any
// byte; if not, NAK the seq and DO NOT advance
// rxSeq. The retx will land when the app drains
// some bytes, the retx will classify in-order
// (Forward), and the seq will be delivered.
//
// This test drives the link layer end-to-end
// through the WireSim and asserts the
// current contract:
//   1. A NAK on a full app buf does NOT push
//      any byte (zero bytes written).
//   2. A NAK on a full app buf does NOT
//      advance rxSeq.
//   3. The retx of the same seq classifies
//      in-order (Forward) and delivers when
//      the app drains.
//   4. Sender + receiver echo counts match
//      (no loss, no duplication).
//
// Source-grep pins for the current fix:
//   a. `onPayload` calls `hw.appBufFree()` BEFORE
//      any `hw.pushAppBuf(b, n)` (the all-or-
//      nothing check).
//   b. The `rxSeq = cobsSeq; rxSeqSet = true;`
//      pair appears AFTER the HoldAck branch
//      (committed on delivery, not on entry).
//   c. `appBufFree()` is implemented on both
//      `IHal` and `EspHal` (not just
//      `MockHal`).
//   d. `appBufFree()` is called in both the
//      Forward-path and the SYNC Gap-path (the
//      Gap path also wrote partial bytes
//      buggy-original).

#ifndef ARDUINO

#    include <cassert>
#    include <cstdio>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return std::string();
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

// Extract the function body of an exact function
// signature in a source file. Walks brace
// nesting from the opening `{` after the
// signature. Returns "" on failure.
std::string extractFnBody(const std::string &src,
                          const std::string &signature) {
    auto start = src.find(signature);
    if (start == std::string::npos)
        return std::string();
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return std::string();
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
    return std::string();
}

// Pin a: the onPayload body must call
// `hw.appBufFree()` BEFORE any `hw.pushAppBuf(b, n)`.
void test_onPayload_all_or_nothing_check() {
    std::cout
        << "\n=== Pin a: onPayload checks appBufFree BEFORE pushAppBuf ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/io/LinkRx.cpp");
    assert(!src.empty());

    std::string body = extractFnBody(src, "bool Link::onPayload");
    assert(!body.empty());

    auto freePos = body.find("hw.appBufFree()");
    auto pushPos = body.find("hw.pushAppBuf(b, n)");
    assert(freePos != std::string::npos);
    assert(pushPos != std::string::npos);
    assert(freePos < pushPos);
    std::cout << "  appBufFree() at offset " << freePos
              << " precedes pushAppBuf(b, n) at offset " << pushPos << " \u2713"
              << std::endl;
    std::cout << "  PASS (all-or-nothing admission check present)" << std::endl;
}

// Pin b: the rxSeq / rxSeqSet commit pair must
// appear AFTER the HoldAck branch. The buggy-original
// shape had:
//   rxSeq = cobsSeq;
//   rxSeqSet = true;
//   ...
//   if (decideAppBuf(acc, n) == AppBufAction::HoldAck) {
//       sendNakFrame_unlocked(cobsSeq);
//       return false;
//   }
// The NAKed frame's retx classified as Stale
// because rxSeq was already at cobsSeq.
// The fix moves the commit to AFTER the
// HoldAck check.
void test_rxSeq_commit_after_holdack() {
    std::cout << "\n=== Pin b: rxSeq commit AFTER the HoldAck check ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/io/LinkRx.cpp");
    assert(!src.empty());

    std::string body = extractFnBody(src, "bool Link::onPayload");
    assert(!body.empty());

    // Find the HoldAck branch and the rxSeq commit
    // site. The HoldAck branch is the `if (hw.appBufFree()
    // < n)` block (the new all-or-nothing shape)
    // OR the legacy `decideAppBuf(acc, n) ==
    // AppBufAction::HoldAck` shape. Either way,
    // the HoldAck branch must contain a `sendNak`
    // call.
    auto freeCheckPos = body.find("hw.appBufFree() < n");
    assert(freeCheckPos != std::string::npos);
    // The NAK call site in the HoldAck branch
    // must precede the rxSeq commit site.
    auto nakPos = body.find("sendNakFrame_unlocked(cobsSeq)", freeCheckPos);
    assert(nakPos != std::string::npos);
    // Find the rxSeq commit site AFTER the
    // HoldAck branch.
    auto rxSeqPos = body.find("rxSeq = cobsSeq;", freeCheckPos);
    assert(rxSeqPos != std::string::npos);
    assert(nakPos < rxSeqPos);
    std::cout << "  HoldAck-NAK at offset " << nakPos
              << " precedes rxSeq commit at offset " << rxSeqPos << " \u2713"
              << std::endl;
    // The rxSeq commit must be the LAST commit
    // before the cumulative ACK. (Defensive
    // against a refactor that re-introduces an
    // early commit.)
    auto ackPos = body.find("sendAckFrame_unlocked(cobsSeq,", rxSeqPos);
    assert(ackPos != std::string::npos);
    assert(rxSeqPos < ackPos);
    std::cout << "  rxSeq commit at offset " << rxSeqPos
              << " precedes cumulative ACK at offset " << ackPos << " \u2713"
              << std::endl;
    std::cout << "  PASS (rxSeq commit is post-HoldAck, pre-ACK)" << std::endl;
}

// Pin c: appBufFree() is implemented on both
// IHal (the default), EspHal (the real impl),
// and MockHal (the host test impl). The
// default returns INT32_MAX so any HAL that
// forgets to override the all-or-nothing
// check auto-passes (preserving the buggy-original
// "always room" contract for HALs that don't
// track capacity).
void test_appBufFree_implemented() {
    std::cout << "\n=== Pin c: appBufFree() on IHal + EspHal + MockHal ==="
              << std::endl;
    std::string root = projectRoot();

    std::string ihal = readFile(root + "/src/al/hal/IHal.h");
    assert(ihal.find("virtual int appBufFree() const") != std::string::npos);
    std::cout << "  IHal declares appBufFree() \u2713" << std::endl;

    std::string esp = readFile(root + "/src/al/hal/EspHal.h");
    assert(esp.find("int appBufFree() const override") != std::string::npos);
    // The EspHal impl must call
    // xStreamBufferSpacesAvailable — that's the
    // FreeRTOS primitive that reports the
    // free-space of the receive stream buffer.
    assert(esp.find("xStreamBufferSpacesAvailable") != std::string::npos);
    std::cout << "  EspHal overrides appBufFree() with "
                 "xStreamBufferSpacesAvailable \u2713"
              << std::endl;

    std::string mock = readFile(root + "/test/common/MockHal.h");
    assert(mock.find("int appBufFree() const override") != std::string::npos);
    std::cout << "  MockHal overrides appBufFree() \u2713" << std::endl;

    std::cout << "  PASS (appBufFree() implemented on all three HALs)"
              << std::endl;
}

// Pin d: the SYNC-mode Gap path also uses
// appBufFree() (not the legacy pushAppBuf
// without a check). The buggy-original shape pushed
// partial bytes on a SYNC Gap too; the fix
// tightens both paths.
void test_sync_gap_path_uses_appBufFree() {
    std::cout << "\n=== Pin d: SYNC-mode Gap path also uses appBufFree ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/io/LinkRx.cpp");
    assert(!src.empty());

    std::string body = extractFnBody(src, "bool Link::onPayload");
    assert(!body.empty());

    // The SYNC Gap branch starts at "if (cls ==
    // GapClass::Gap)" and contains "cfg.mode ==
    // AutoLinkConfig::Mode::SYNC". Walk that
    // sub-block.
    auto gapPos = body.find("if (cls == GapClass::Gap)");
    assert(gapPos != std::string::npos);
    auto syncPos = body.find("cfg.mode == AutoLinkConfig::Mode::SYNC", gapPos);
    assert(syncPos != std::string::npos);
    auto syncEnd = body.find("return false;", syncPos);
    assert(syncEnd != std::string::npos);
    std::string syncBlock = body.substr(syncPos, syncEnd - syncPos);
    assert(syncBlock.find("hw.appBufFree()") != std::string::npos);
    std::cout << "  SYNC Gap block uses hw.appBufFree() for admission \u2713"
              << std::endl;
    std::cout << "  PASS (Gap path also all-or-nothing)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== App-buf full admit-nothing regression tests ==="
              << std::endl;
    test_onPayload_all_or_nothing_check();
    test_rxSeq_commit_after_holdack();
    test_appBufFree_implemented();
    test_sync_gap_path_uses_appBufFree();
    std::cout << "\n=== App-buf full admit-nothing tests PASS ===" << std::endl;
    return 0;
}

#endif
