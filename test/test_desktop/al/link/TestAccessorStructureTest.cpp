// Link and AutoLink do not expose test_*
// / *_ForTest methods on the production
// public API. Test access is through
// LinkTestAccessor and AutoLinkTestAccessor
// shims under test/common/, which are
// friends of the production classes.
//
// Regression pin: this test reads the
// Link.h and AutoLink.h source files and
// asserts the public sections (anything
// after a `public:` line, up to the next
// `private:`) do not contain the
// `test_*` or `*_ForTest` method
// declarations that used to live there.
// The shim boundary is the only path
// tests reach internals through.
//
// If a test_* method is added back to
// the public section of either class,
// this test goes red. The companion
// shim existence check fails the
// compile if either shim is removed
// from test/common/.
//
// Source-level only — there's no host
// coverage to construct a Link/AutoLink
// against, the shim boundary IS the
// pin.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/link/Link.h"
#include "AutoLink.h"
#include "LinkTestAccessor.h"
#include "AutoLinkTestAccessor.h"

#include <iostream>
#include <cassert>
#include <fstream>
#include <string>
#include <vector>
#include "NullArqCache.h"

using namespace autolink;

namespace {

// Split file into sections by access
// specifier. Returns the union of public
// section bodies (between `public:` and
// the next `private:` or end-of-class).
std::vector<std::string> publicSectionBodies(const std::string &src) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t pub = src.find("public:", pos);
        if (pub == std::string::npos)
            break;
        size_t end = src.find("private:", pub + 7);
        if (end == std::string::npos)
            end = src.size();
        out.emplace_back(src.substr(pub, end - pub));
        pos = end;
    }
    return out;
}

// True if any public section body
// contains the literal as a method
// declaration. The check is
// substring-based: a `test_markAckedPending`
// anywhere in a public body is a fail.
// Comments are NOT filtered — keep
// those clean too.
bool publicBodyContainsMethod(const std::vector<std::string> &sections,
                              const std::string &needle) {
    for (const auto &s : sections)
        if (s.find(needle) != std::string::npos)
            return true;
    return false;
}

// True if any public section body
// declares a method matching the
// pattern "test_*(...)". A public
// `test_TEMPORARY_BREAK()` slips past
// the literal-name check; this one
// catches it.
bool publicBodyDeclaresAnyTestHook(const std::vector<std::string> &sections) {
    // Pattern: "test_" followed by an
    // identifier, then an opening
    // paren. The "test_" is anchored
    // to a word boundary to avoid
    // matching identifiers that
    // happen to contain "test_" as
    // a substring.
    for (const auto &s : sections) {
        size_t pos = 0;
        while ((pos = s.find("test_", pos)) != std::string::npos) {
            // Require word boundary:
            // previous char is non-identifier
            // (or start of string) AND
            // next char is a letter or
            // underscore.
            bool prevOk = (pos == 0) ||
                !(isalnum((unsigned char)s[pos - 1]) || s[pos - 1] == '_');
            bool nextOk = pos + 5 < s.size() &&
                (isalpha((unsigned char)s[pos + 5]) || s[pos + 5] == '_');
            if (!prevOk || !nextOk) {
                pos++;
                continue;
            }
            // Require an opening paren
            // somewhere soon (within the
            // next 80 chars — enough to
            // skip over return type and
            // parameter names).
            size_t paren = s.find('(', pos);
            if (paren == std::string::npos || paren - pos > 80) {
                pos++;
                continue;
            }
            return true;
        }
    }
    return false;
}

std::string slurp(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "FAIL: cannot open " << path << std::endl;
        assert(false);
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

} // namespace

void test_link_public_no_test_markAckedPending() {
    std::cout << "\n=== Test: Link public API has no test_markAckedPending "
                 "==="
              << std::endl;
    std::string src = slurp("../../src/al/link/Link.h");
    auto pubs = publicSectionBodies(src);
    assert(!publicBodyContainsMethod(pubs, "test_markAckedPending"));
    std::cout << "  Link::test_markAckedPending is private ✓" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_link_public_no_test_sendMsgBegin() {
    std::cout << "\n=== Test: Link public API has no test_sendMsgBegin "
                 "==="
              << std::endl;
    std::string src = slurp("../../src/al/link/Link.h");
    auto pubs = publicSectionBodies(src);
    assert(!publicBodyContainsMethod(pubs, "test_sendMsgBegin"));
    assert(!publicBodyContainsMethod(pubs, "test_sendMsgStillWaiting"));
    assert(!publicBodyContainsMethod(pubs, "test_reorderSlotInUse"));
    assert(!publicBodyContainsMethod(pubs, "test_reorderSlotLen"));
    assert(!publicBodyContainsMethod(pubs, "arqCacheForTest"));
    // Catch-all: any new public test_*
    // method slips past the literal
    // list above. The lockdown is on
    // the whole test_* namespace in
    // the public API.
    assert(!publicBodyDeclaresAnyTestHook(pubs));
    std::cout << "  Link::test_sendMsgBegin is private ✓" << std::endl;
    std::cout << "  Link::test_sendMsgStillWaiting is private ✓" << std::endl;
    std::cout << "  Link::test_reorderSlotInUse is private ✓" << std::endl;
    std::cout << "  Link::test_reorderSlotLen is private ✓" << std::endl;
    std::cout << "  Link::arqCacheForTest is private ✓" << std::endl;
    std::cout << "  no public test_* method declarations ✓" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_autolink_public_no_test_hooks() {
    std::cout << "\n=== Test: AutoLink public API has no test_* / *_ForTest "
                 "hooks ==="
              << std::endl;
    std::string src = slurp("../../include/AutoLink.h");
    auto pubs = publicSectionBodies(src);
    assert(!publicBodyContainsMethod(pubs, "test_arqCache_put"));
    assert(!publicBodyContainsMethod(pubs, "test_arqCache_hasRoom"));
    assert(!publicBodyContainsMethod(pubs, "test_arqPoolSize"));
    assert(!publicBodyContainsMethod(pubs, "test_arqCache_freeBySeq"));
    assert(!publicBodyContainsMethod(pubs, "test_arqCache_retx"));
    assert(!publicBodyContainsMethod(pubs, "test_arqCache_findBySeq"));
    assert(!publicBodyContainsMethod(pubs, "test_markAckedPending"));
    assert(!publicBodyContainsMethod(pubs, "test_sendMsgBeginForTest"));
    assert(!publicBodyContainsMethod(pubs, "test_sendMsgStillWaitingForTest"));
    assert(!publicBodyContainsMethod(pubs, "syncAckTimeoutMsForTest"));
    assert(!publicBodyContainsMethod(pubs, "linkForTest"));
    assert(!publicBodyContainsMethod(pubs, "arqCacheForTest"));
    assert(!publicBodyContainsMethod(pubs, "arqCacheSizeForTest"));
    // Catch-all for the test_* and
    // *_ForTest namespaces.
    assert(!publicBodyDeclaresAnyTestHook(pubs));
    for (const auto &s : pubs) {
        assert(s.find("ForTest") == std::string::npos);
    }
    std::cout << "  AutoLink facade has no test_* / *_ForTest public "
                 "hooks ✓"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_shims_compile_and_exist() {
    std::cout << "\n=== Test: shim classes compile + exist ===" << std::endl;
    // The shim is the only path tests
    // reach the internals through. If
    // either shim is removed from
    // test/common/, every host test
    // stops compiling — that's the
    // loud regression signal. A static
    // assert at file scope fails the
    // compile if the type is missing.
    static_assert(sizeof(LinkTestAccessor) > 0, "LinkTestAccessor must exist");
    static_assert(sizeof(AutoLinkTestAccessor) > 0,
                  "AutoLinkTestAccessor must exist");
    std::cout << "  LinkTestAccessor exists ✓" << std::endl;
    std::cout << "  AutoLinkTestAccessor exists ✓" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_friend_decls_in_place() {
    std::cout << "\n=== Test: friend declarations in place ===" << std::endl;
    // The shim's authority to reach
    // Link's privates comes from
    // `friend class LinkTestAccessor`.
    // If that line is removed from
    // Link.h, every host test that
    // builds the shim fails to
    // compile. Pin it as a source
    // substring.
    std::string linkSrc = slurp("../../src/al/link/Link.h");
    assert(linkSrc.find("friend class LinkTestAccessor") != std::string::npos);
    std::cout << "  Link::friend class LinkTestAccessor present ✓" << std::endl;

    std::string facadeSrc = slurp("../../include/AutoLink.h");
    assert(facadeSrc.find("friend class AutoLinkTestAccessor") !=
           std::string::npos);
    std::cout << "  AutoLink::friend class AutoLinkTestAccessor present ✓"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_link_dedup_helpers_present() {
    std::cout << "\n=== Test: Link.cpp dedup helpers present ===" << std::endl;
    // Five copy-paste fixes collapsed
    // into helpers + one constant
    // removal. Pins:
    //   1. buildAndTxCobsFrame_unlocked
    //      declared + defined.
    //   2. sendCtrlCobsFrame_unlocked
    //      declared + defined.
    //   3. buildAndSendMsg_unlocked
    //      declared + defined.
    //   4. processCtrlFrame_unlocked
    //      declared + defined.
    //   5. LOCK_CMD_BASE removed.
    //   6. sendCobsFrame_unlocked no
    //      longer inlines UtilCobs::encode.
    //   7. resendCobsFrame_unlocked no
    //      longer inlines UtilCobs::encode.
    //   8. sendAckFrame_unlocked no
    //      longer inlines UtilCobs::encode.
    //   9. sendNakFrame_unlocked no
    //      longer inlines UtilCobs::encode.
    //   10. onRx no longer inlines
    //       crc8(rxBuf, ...).
    //   11. LOCK_CMD range-check site
    //       in Link.cpp uses LOCK_CMD
    //       (not LOCK_CMD_BASE).
    std::string linkH = slurp("../../src/al/link/Link.h");
    // The Link implementation is split across six
    // translation units (LinkCore / LinkTx / LinkRx /
    // LinkSweep / LinkTimers / LinkApi) since the
    // god-class split A test that grep'd
    // Link.cpp for a method body would silently miss
    // the move; concat every TU into one virtual
    // "Link.cpp" string and search that.
    std::string linkCpp;
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        linkCpp += slurp(std::string("../../src/al/link/") + tu);
    }

    assert(linkH.find("buildAndTxCobsFrame_unlocked") != std::string::npos);
    assert(linkCpp.find("Link::buildAndTxCobsFrame_unlocked(") !=
           std::string::npos);
    std::cout << "  buildAndTxCobsFrame_unlocked declared+defined ✓"
              << std::endl;

    assert(linkH.find("sendCtrlCobsFrame_unlocked") != std::string::npos);
    assert(linkCpp.find("Link::sendCtrlCobsFrame_unlocked(") !=
           std::string::npos);
    std::cout << "  sendCtrlCobsFrame_unlocked declared+defined ✓" << std::endl;

    assert(linkH.find("buildAndSendMsg_unlocked") != std::string::npos);
    assert(linkCpp.find("Link::buildAndSendMsg_unlocked(") !=
           std::string::npos);
    std::cout << "  buildAndSendMsg_unlocked declared+defined ✓" << std::endl;

    assert(linkH.find("processCtrlFrame_unlocked") != std::string::npos);
    assert(linkCpp.find("Link::processCtrlFrame_unlocked(") !=
           std::string::npos);
    std::cout << "  processCtrlFrame_unlocked declared+defined ✓" << std::endl;

    assert(linkH.find("LOCK_CMD_BASE") == std::string::npos);
    assert(linkCpp.find("LOCK_CMD_BASE") == std::string::npos);
    std::cout << "  LOCK_CMD_BASE removed ✓" << std::endl;

    // Pin the delegation: the old call
    // sites must not inline UtilCobs::encode
    // any more. The new helpers do.
    // (Use a brace-balanced slice of the
    // function body to scope the check;
    // a substring scan over the whole
    // file would false-positive on the
    // helper itself.)
    auto bodyOf = [&linkCpp](const std::string &fnName) -> std::string {
        std::string needle = "void Link::" + fnName + "(";
        size_t start = linkCpp.find(needle);
        if (start == std::string::npos)
            start = linkCpp.find("bool Link::" + fnName + "(");
        if (start == std::string::npos)
            return std::string();
        // Find opening brace, then the
        // matching close. The bodies
        // here don't have nested
        // braces in our targets, but
        // walk the depth to be safe.
        size_t ob = linkCpp.find('{', start);
        if (ob == std::string::npos)
            return std::string();
        int depth = 0;
        size_t i = ob;
        for (; i < linkCpp.size(); ++i) {
            if (linkCpp[i] == '{')
                depth++;
            else if (linkCpp[i] == '}') {
                depth--;
                if (depth == 0)
                    break;
            }
        }
        return linkCpp.substr(ob, i - ob + 1);
    };

    std::string sendBody = bodyOf("sendCobsFrame_unlocked");
    assert(!sendBody.empty());
    assert(sendBody.find("UtilCobs::encode") == std::string::npos);
    std::cout << "  sendCobsFrame_unlocked delegates (no inline encode) ✓"
              << std::endl;

    std::string resendBody = bodyOf("resendCobsFrame_unlocked");
    assert(!resendBody.empty());
    assert(resendBody.find("UtilCobs::encode") == std::string::npos);
    std::cout << "  resendCobsFrame_unlocked delegates (no inline encode) ✓"
              << std::endl;

    std::string ackBody = bodyOf("sendAckFrame_unlocked");
    assert(!ackBody.empty());
    // this release: sendAckFrame_unlocked produces a 5-byte
    // ACK payload (extended with bytes-recvd) so it
    // inlines UtilCobs::encode rather than routing
    // through sendCtrlCobsFrame_unlocked (which still
    // emits the 3-byte NAK frame). Pin the 5-byte
    // shape instead of the delegation contract.
    assert(ackBody.find("ACK_TYPE") != std::string::npos);
    assert(ackBody.find("bytesRecvd") != std::string::npos);
    std::cout
        << "  sendAckFrame_unlocked emits 5-byte ACK (type+seq+bytes+CRC) \u2713"
        << std::endl;

    std::string nakBody = bodyOf("sendNakFrame_unlocked");
    assert(!nakBody.empty());
    assert(nakBody.find("UtilCobs::encode") == std::string::npos);
    std::cout << "  sendNakFrame_unlocked delegates (no inline encode) ✓"
              << std::endl;

    // onRx should not contain a direct
    // crc8(rxBuf, ...) call any more
    // — that path is in
    // processCtrlFrame_unlocked.
    std::string onRxBody = bodyOf("onRx");
    assert(!onRxBody.empty());
    assert(onRxBody.find("crc8(rxBuf,") == std::string::npos);
    std::cout << "  onRx delegates crc8(rxBuf,...) to "
                 "processCtrlFrame_unlocked ✓"
              << std::endl;

    // The LOCK_CMD range-check used to be inlined in
    // handleSwp_unlocked with three LOCK_CMD sites
    // (>= LOCK_CMD / < LOCK_CMD + / - LOCK_CMD). Now it
    // lives in the LinkDecision pure-function
    // isLockPayload() so the decision is testable in
    // isolation. Pin the helper exists + is called.
    assert(linkCpp.find("isLockPayload(") != std::string::npos);
    std::cout << "  LOCK_CMD decode delegated to isLockPayload ✓" << std::endl;

    std::cout << "PASS" << std::endl;
}

void test_choke_points_route_through_clamped_accessors() {
    // Regression pin for the OOB-closed shape:
    // the raw cfg.allowedBauds[i] read pattern used
    // to be reachable even when a sketch wrote
    // cfg.allowedBaudsCount = 20 post-construction
    // (the field is public for back-compat, so the
    // read paths were the only safe place to
    // enforce the bound). Now the link layer reads
    // through cfg.allowedBaudSafe(i) /
    // cfg.clampedCount() at every choke point —
    // any future regression that drops back to the
    // raw cfg.allowedBauds[] or cfg.allowedBaudsCount
    // read trips here.
    std::cout << "\n=== Test: link choke-points route through clamped "
                 "accessors ==="
              << std::endl;
    std::string linkH = slurp("../../src/al/link/Link.h");
    std::string cfgH = slurp("../../src/al/AutoLinkConfig.h");

    // The choke points in Link.h override the
    // ILinkContext interface (private override
    // section). Pin the body shapes directly
    // instead of public-section scoping.
    size_t bStart = linkH.find("allowedBaudsCount() const override");
    assert(bStart != std::string::npos);
    // Find the matching body close — Link's
    // overrides are inline single-return.
    size_t bBrace = linkH.find('{', bStart);
    size_t bClose = linkH.find('}', bBrace);
    std::string countBody = linkH.substr(bStart, bClose - bStart + 1);
    assert(countBody.find("AUTOLINK_MAX_BAUDS") != std::string::npos);
    std::cout << "  Link::allowedBaudsCount() clamps to AUTOLINK_MAX_BAUDS \u2713"
              << std::endl;

    size_t eStart = linkH.find("allowedBaud(int i) const override");
    assert(eStart != std::string::npos);
    size_t eBrace = linkH.find('{', eStart);
    size_t eClose = linkH.find('}', eBrace);
    std::string baudBody = linkH.substr(eStart, eClose - eStart + 1);
    assert(baudBody.find("cfg.allowedBaudSafe(i)") != std::string::npos);
    std::cout
        << "  Link::allowedBaud(i) routes through cfg.allowedBaudSafe(i) \u2713"
        << std::endl;

    // Every Link* TU that indexed cfg.allowedBauds[i]
    // raw must now go through cfg.allowedBaudSafe(i).
    // (We grep the body TUs because that's where the
    // index sites live.)
    std::string linkCpp;
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        linkCpp += slurp(std::string("../../src/al/link/") + tu);
    }
    assert(linkCpp.find("cfg.allowedBauds[") == std::string::npos);
    std::cout << "  no raw cfg.allowedBauds[i] reads in Link body TUs \u2713"
              << std::endl;
    assert(linkCpp.find("cfg.allowedBaudsCount") == std::string::npos);
    std::cout
        << "  no raw cfg.allowedBaudsCount reads in Link body TUs \u2713"
        << std::endl;

    // AutoLinkConfig must expose the two safe
    // accessors that the choke points route through.
    assert(cfgH.find("clampedCount()") != std::string::npos);
    std::cout << "  AutoLinkConfig::clampedCount() exists \u2713" << std::endl;
    assert(cfgH.find("allowedBaudSafe(int i)") != std::string::npos);
    std::cout << "  AutoLinkConfig::allowedBaudSafe(i) exists \u2713"
              << std::endl;

    std::cout << "PASS" << std::endl;
}

void test_msg_hdr_consistency_in_linktx() {
    // Regression pin for the literal-6 / MSG_HDR drift:
    // LinkTx::buildAndTxCobsFrame_unlocked used to
    // size its stack frame as uint8_t frame[MAX_CHUNK
    // + 6]; — a parallel literal that wouldn't track
    // a future MSG_HDR bump. After the fix it reads
    // MAX_CHUNK + MSG_HDR so the buffer tracks the
    // same constant every other TU asserts on.
    std::cout
        << "\n=== Test: LinkTx uses MSG_HDR (not literal 6) for frame buf ==="
        << std::endl;
    std::string linkTx = slurp("../../src/al/link/LinkTx.cpp");
    // Find the body of buildAndTxCobsFrame_unlocked.
    size_t start = linkTx.find("void Link::buildAndTxCobsFrame_unlocked(");
    assert(start != std::string::npos);
    size_t ob = linkTx.find('{', start);
    assert(ob != std::string::npos);
    int depth = 0;
    size_t i = ob;
    for (; i < linkTx.size(); ++i) {
        if (linkTx[i] == '{')
            depth++;
        else if (linkTx[i] == '}') {
            depth--;
            if (depth == 0)
                break;
        }
    }
    std::string body = linkTx.substr(ob, i - ob + 1);
    assert(body.find("MAX_CHUNK + MSG_HDR") != std::string::npos);
    std::cout << "  buildAndTxCobsFrame_unlocked uses MAX_CHUNK + MSG_HDR \u2713"
              << std::endl;
    assert(body.find("MAX_CHUNK + 6") == std::string::npos);
    std::cout << "  no literal 6 in the stack frame size \u2713" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_ihal_setevents_guard_in_place() {
    // Regression pin: IHal::setEvents used to be a
    // plain pointer assignment with a comment that
    // claimed "a second setEvents is an assertion
    // fail" — but there was no assert. The guard was
    // added in this release (host assert + on-device
    // log). Pin both shapes so the contract doesn't
    // silently degrade to plain assignment again.
    std::cout << "\n=== Test: IHal::setEvents asserts on double-bind ==="
              << std::endl;
    std::string halH = slurp("../../src/al/hal/IHal.h");
    // The host assertion. AUTOLINK_HOST_TEST is the
    // build flag the test gate uses; the real device
    // build doesn't see this branch.
    assert(halH.find("#ifdef AUTOLINK_HOST_TEST") != std::string::npos);
    assert(halH.find("assert(events_ == nullptr") != std::string::npos);
    std::cout << "  host-build assert on double setEvents \u2713" << std::endl;
    // The on-device log path.
    assert(halH.find("Log::log().error(\"IHal\"") != std::string::npos);
    std::cout << "  on-device log on double setEvents \u2713" << std::endl;
    // The comment must match the contract — no
    // claim of an assertion without one present.
    assert(halH.find("an assertion fail") == std::string::npos);
    std::cout << "  setEvents comment no longer claims a phantom assert \u2713"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== TestAccessor Structure ===" << std::endl;
    test_link_public_no_test_markAckedPending();
    test_link_public_no_test_sendMsgBegin();
    test_autolink_public_no_test_hooks();
    test_shims_compile_and_exist();
    test_friend_decls_in_place();
    test_link_dedup_helpers_present();
    test_choke_points_route_through_clamped_accessors();
    test_msg_hdr_consistency_in_linktx();
    test_ihal_setevents_guard_in_place();
    std::cout << "\n=== TestAccessor Structure Completed ===" << std::endl;
    return 0;
}
