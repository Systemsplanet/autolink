// AutoLinkWebTest.cpp — host tests for the dashboard core.
//
// These tests verify the OBSERVABLE behavior of the web monitor
// without needing an ESP32:
//   * /stats JSON format
//   * /logs JSON format (including the `head` field for backlog skip)
//   * /level query parsing (NONE rejection, out-of-range, valid)
//   * /mode query parsing
//   * Log level application
//   * Role string validation
//   * Dashboard HTML contains the role-conditional UI elements
//     (Ping-only class on Pause/Resume + fill-mode radio, body[data-role]
//      toggling in the JS, Reboot button at top)
//
// If a future change to the wire-format / dashboard behavior breaks
// the protocol or the JS, these tests will catch it BEFORE the user
// ever downloads the firmware.

#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <cstring>
#include <string>
#include "Log.h"
#include "ALink.h"
#include "AutoLink.h"
#include "AutoLinkWebCore.h"
#include "AutoLinkWebHtml.h"
#include "MockHal.h"

using namespace autolink;

// ---- /stats JSON format -------------------------------------------------

// All 14 fields of WebSnapshot must appear in the /stats JSON output.
// Catches drift if someone removes a field from formatStatsJson or
// from WebSnapshot without updating both. (This was the bug fixed in
// v5.1.13: the old `Snapshot` in AutoLinkWeb.h and `WebSnapshot` in
// AutoLinkWebCore.h were separate structs that had drifted apart over
// time. The fix unified them.)
void test_stats_json_has_all_14_fields() {
    std::cout << "\n=== Test: /stats JSON has all 14 WebSnapshot fields ===" << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    s.errCount = 1;
    s.txBps    = 11;
    s.rxBps    = 22;
    s.txTotal  = 100;
    s.rxTotal  = 200;
    s.errTotal = 3;
    s.lostMsgs = 4;
    s.rssi     = -50;
    s.freeHeap = 99999;
    s.uptimeS  = 60;
    s.baudRate = 115200;
    s.fillMode = 1;
    std::strcpy(s.role, "Ping");
    char buf[512];
    int len = formatStatsJson(&s, 3, "5.1.13", buf, sizeof(buf));
    assert(len > 0);
    // Every field name must appear in the output.
    const char* mustHave[] = {
        "\"state\":", "\"errCount\":", "\"txBps\":", "\"rxBps\":",
        "\"txTotal\":", "\"rxTotal\":", "\"errTotal\":", "\"lostMsgs\":",
        "\"rssi\":", "\"freeHeap\":", "\"uptimeS\":", "\"baudRate\":",
        "\"lvl\":", "\"mode\":", "\"role\":", "\"version\":",
        nullptr
    };
    for (int i = 0; mustHave[i]; i++) {
        assert(std::strstr(buf, mustHave[i]) != nullptr);
    }
    // The mode field must reflect the input value (1 == random).
    assert(std::strstr(buf, "\"mode\":1") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_stats_json_format() {
    std::cout << "\n=== Test: /stats JSON Format ===" << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    s.errCount = 42;
    s.txBps    = 1024;
    s.rxBps    = 2048;
    s.txTotal  = 1000000;
    s.rxTotal  = 2000000;
    s.errTotal = 5;
    s.lostMsgs = 0;
    s.rssi     = -65;
    s.freeHeap = 200000;
    s.uptimeS  = 3600;
    s.baudRate = 115200;
    s.fillMode = 0;
    std::strcpy(s.role, "Ping");

    char buf[512];
    int  len = formatStatsJson(&s, 3, "5.0.7", buf, sizeof(buf));
    assert(len > 0);
    assert(len < (int)sizeof(buf));

    // Every documented field must be present, exact name, exact type.
    assert(std::strstr(buf, "\"state\":\"OK\"")      != nullptr);
    assert(std::strstr(buf, "\"errCount\":42")       != nullptr);
    assert(std::strstr(buf, "\"txBps\":1024")        != nullptr);
    assert(std::strstr(buf, "\"rxBps\":2048")        != nullptr);
    assert(std::strstr(buf, "\"txTotal\":1000000")   != nullptr);
    assert(std::strstr(buf, "\"rxTotal\":2000000")   != nullptr);
    assert(std::strstr(buf, "\"errTotal\":5")        != nullptr);
    assert(std::strstr(buf, "\"rssi\":-65")          != nullptr);
    assert(std::strstr(buf, "\"freeHeap\":200000")   != nullptr);
    assert(std::strstr(buf, "\"uptimeS\":3600")      != nullptr);
    assert(std::strstr(buf, "\"baudRate\":115200")   != nullptr);
    assert(std::strstr(buf, "\"lvl\":3")             != nullptr);
    assert(std::strstr(buf, "\"mode\":0")            != nullptr);
    assert(std::strstr(buf, "\"role\":\"Ping\"")     != nullptr);
    assert(std::strstr(buf, "\"version\":\"5.0.7\"") != nullptr);

    // Well-formed JSON: starts with { and ends with }.
    assert(buf[0] == '{');
    assert(buf[len - 1] == '}');
    std::cout << "PASS" << std::endl;
}

void test_stats_json_with_pong_role() {
    std::cout << "\n=== Test: /stats JSON Includes 'Pong' Role ===" << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    std::strcpy(s.role, "Pong");
    char buf[512];
    int len = formatStatsJson(&s, 3, "5.0.7", buf, sizeof(buf));
    assert(std::strstr(buf, "\"role\":\"Pong\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_stats_json_with_empty_role() {
    std::cout << "\n=== Test: /stats JSON With Empty Role (Legacy Code) ===" << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "SWP");
    s.role[0] = '\0';
    char buf[512];
    int len = formatStatsJson(&s, 4, "5.0.7", buf, sizeof(buf));
    assert(std::strstr(buf, "\"role\":\"\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

// ---- /level query parsing -----------------------------------------------
void test_level_query_valid() {
    std::cout << "\n=== Test: parseLevelQuery Valid Levels ===" << std::endl;
    assert(parseLevelQuery("1") == 1);
    assert(parseLevelQuery("2") == 2);
    assert(parseLevelQuery("3") == 3);
    assert(parseLevelQuery("4") == 4);
    assert(parseLevelQuery("5") == 5);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_none() {
    std::cout << "\n=== Test: parseLevelQuery Rejects lv=0 (NONE) ===" << std::endl;
    // lv=0 is rejected because it silences the logger and is
    // unrecoverable without reflash. -2 is the documented return
    // for this case (vs -3 for out-of-range).
    assert(parseLevelQuery("0") == -2);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_out_of_range() {
    std::cout << "\n=== Test: parseLevelQuery Rejects Out-of-Range ===" << std::endl;
    // Above VERBOSE: -3
    assert(parseLevelQuery("6")  == -3);
    assert(parseLevelQuery("99") == -3);
    // Below ERROR (lv=-1): caught as < 0, so -3
    assert(parseLevelQuery("-1") == -3);
    // atoi("abc") = 0 → caught as NONE rejection → -2
    assert(parseLevelQuery("abc") == -2);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_empty() {
    std::cout << "\n=== Test: parseLevelQuery Rejects Empty/Missing ===" << std::endl;
    assert(parseLevelQuery("")    == -1);
    assert(parseLevelQuery(nullptr) == -1);
    std::cout << "PASS" << std::endl;
}

// ---- applyLogLevel ------------------------------------------------------
void test_apply_log_level() {
    std::cout << "\n=== Test: applyLogLevel Mutates Log Singleton ===" << std::endl;
    Log& L = Log::getLog();
    L.setLevel(Log::INFO);
    int rc = applyLogLevel(Log::DEBUG);
    assert(rc == (int)Log::DEBUG);
    assert(L.getLevel() == Log::DEBUG);
    // Reject NONE.
    rc = applyLogLevel(Log::NONE);
    assert(rc == -1);
    assert(L.getLevel() == Log::DEBUG); // unchanged
    // Out of range.
    rc = applyLogLevel(99);
    assert(rc == -2);
    assert(L.getLevel() == Log::DEBUG); // unchanged
    std::cout << "PASS" << std::endl;
}

// ---- /mode query parsing ------------------------------------------------
void test_mode_query() {
    std::cout << "\n=== Test: parseModeQuery ===" << std::endl;
    assert(parseModeQuery("seq")      == 0);
    assert(parseModeQuery("rand")     == 1);
    assert(parseModeQuery("SEQ")      == -1); // case sensitive
    assert(parseModeQuery("")         == -1);
    assert(parseModeQuery(nullptr)    == -1);
    std::cout << "PASS" << std::endl;
}

// ---- role string validation --------------------------------------------
void test_role_string() {
    std::cout << "\n=== Test: validRoleString ===" << std::endl;
    // role[8] in WebSnapshot. validRoleString returns true for any
    // non-empty string of length 0..7 (n < 8). The NUL terminator
    // is not counted in strlen, so a 7-char string is at the limit.
    assert(validRoleString("Ping")     == true);
    assert(validRoleString("Pong")     == true);
    assert(validRoleString("")         == false);
    assert(validRoleString(nullptr)    == false);
    assert(validRoleString("ABCDEF")   == true);    // 6 chars — fits
    assert(validRoleString("ABCDEFG")  == true);    // 7 chars — at limit
    assert(validRoleString("ABCDEFGH") == false);   // 8 chars — overflow
    std::cout << "PASS" << std::endl;
}

// ---- /logs JSON format --------------------------------------------------
void test_logs_json_empty() {
    std::cout << "\n=== Test: formatLogsJson Empty Ring ===" << std::endl;
    WebLogEntry ring[10] = {};
    char buf[256];
    int len = formatLogsJson(ring, 0, 0, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":0") != nullptr);
    assert(std::strstr(buf, "\"lines\":[]") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_logs_json_with_entries() {
    std::cout << "\n=== Test: formatLogsJson With Entries ===" << std::endl;
    WebLogEntry ring[10] = {};
    ring[0].seq = 0; ring[0].sev = 'I';
    std::strcpy(ring[0].line, "12:34:56.789 I ALinkWeb hello");
    ring[1].seq = 1; ring[1].sev = 'W';
    std::strcpy(ring[1].line, "12:34:57.000 W ALinkWeb warning");
    ring[2].seq = 2; ring[2].sev = 'E';
    std::strcpy(ring[2].line, "12:34:58.000 E ALinkWeb \"quoted\" msg");

    char buf[2048];
    int len = formatLogsJson(ring, 3, 0, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":3") != nullptr);
    assert(std::strstr(buf, "\"seq\":0")   != nullptr);
    assert(std::strstr(buf, "\"seq\":1")   != nullptr);
    assert(std::strstr(buf, "\"seq\":2")   != nullptr);
    assert(std::strstr(buf, "\"sev\":\"I\"") != nullptr);
    assert(std::strstr(buf, "\"sev\":\"W\"") != nullptr);
    assert(std::strstr(buf, "\"sev\":\"E\"") != nullptr);
    // Quoted string with embedded quotes is JSON-escaped.
    assert(std::strstr(buf, "\\\"quoted\\\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_logs_json_respects_since() {
    std::cout << "\n=== Test: formatLogsJson Respects since ===" << std::endl;
    WebLogEntry ring[10] = {};
    ring[0].seq = 0; ring[0].sev = 'I'; std::strcpy(ring[0].line, "first");
    ring[1].seq = 1; ring[1].sev = 'I'; std::strcpy(ring[1].line, "second");
    ring[2].seq = 2; ring[2].sev = 'I'; std::strcpy(ring[2].line, "third");

    char buf[2048];
    // since=1 → skip seq 0, return seqs 1 and 2
    int len = formatLogsJson(ring, 3, 1, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":3")  != nullptr);
    assert(std::strstr(buf, "\"first\"")  == nullptr);
    assert(std::strstr(buf, "\"second\"") != nullptr);
    assert(std::strstr(buf, "\"third\"")  != nullptr);
    std::cout << "PASS" << std::endl;
}

// ---- HTML structure: role-conditional UI -------------------------------
// As of v5.1.6, only Ping-only controls carry .ping-only:
//   - modeGroup (Sequential/Random radio)        — Ping-side control
//   - topPbtn  (message updates pause)           — pauses Ping's send loop
// Log-scroll pause buttons (pbtn, pbtn2) were REMOVED from .ping-only
// in v5.1.6 because pausing log scroll is a read-only action that
// makes sense on both Ping and Pong. See docVersion.md v5.1.6.
void test_html_has_ping_only_class() {
    std::cout << "\n=== Test: HTML has .ping-only class on Ping-only controls (not on log-pause) ===" << std::endl;
    const char* html = DASHBOARD_HTML;
    // Sequential/Random radio — Ping-only.
    assert(std::strstr(html, "id=\"modeGroup\"") != nullptr);
    assert(std::strstr(html, "class=\"lvl-group ping-only\" id=\"modeGroup\"") != nullptr);
    // Header message-updates Pause/Resume — Ping-only.
    assert(std::strstr(html, "id=\"topPbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn pause ping-only\" id=\"topPbtn\"") != nullptr);
    // Live Log Pause button (pbtn) — NOT ping-only, visible on Pong too.
    assert(std::strstr(html, "id=\"pbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn\" id=\"pbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn ping-only\" id=\"pbtn\"") == nullptr);
    // Log overlay Pause button (pbtn2) — NOT ping-only.
    assert(std::strstr(html, "id=\"pbtn2\"") != nullptr);
    assert(std::strstr(html, "class=\"btn\" id=\"pbtn2\"") != nullptr);
    assert(std::strstr(html, "class=\"btn ping-only\" id=\"pbtn2\"") == nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_data_role_toggle() {
    std::cout << "\n=== Test: HTML has CSS to hide .ping-only on Pong ===" << std::endl;
    const char* html = DASHBOARD_HTML;
    // CSS rule: body[data-role="pong"] .ping-only { display: none }
    assert(std::strstr(html, "body[data-role=\"pong\"] .ping-only") != nullptr);
    // JS sets data-role based on d.role
    assert(std::strstr(html, "document.body.setAttribute('data-role','ping')") != nullptr);
    assert(std::strstr(html, "document.body.setAttribute('data-role','pong')") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_reboot_at_top() {
    std::cout << "\n=== Test: Reboot Button is in Header (Top of GUI) ===" << std::endl;
    const char* html = DASHBOARD_HTML;
    // The Reboot button must be in the <header> block, not the Counters row.
    // Pin the ID we use (rebootBtnTop) and confirm it appears in the
    // header (before the </header> tag).
    const char* rebootBtn = std::strstr(html, "id=\"rebootBtnTop\"");
    assert(rebootBtn != nullptr);
    const char* headerEnd = std::strstr(html, "</header>");
    assert(headerEnd != nullptr);
    assert(rebootBtn < headerEnd);
    // And the old reboot button in the Counters row should be GONE.
    assert(std::strstr(html, "id=\"rebootBtn\"") == nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_correct_timeouts() {
    std::cout << "\n=== Test: JS fetch timeout is 5s (not 2.5s) ===" << std::endl;
    const char* html = DASHBOARD_HTML;
    // The default and per-call timeouts in tfetch() should be 5000.
    assert(std::strstr(html, "ms||5000") != nullptr);
    // And no leftover 2500.
    assert(std::strstr(html, "2500") == nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_skips_backlog_on_first_poll() {
    std::cout << "\n=== Test: JS Skips Backlog on First Poll ===" << std::endl;
    const char* html = DASHBOARD_HTML;
    // The /logs handler returns a JSON object with a `head` field
    // and the JS reads d2.head to skip the boot-time backlog. The
    // JSON literal `head` is generated server-side; verify the JS
    // uses it.
    assert(std::strstr(html, "d2.head!==undefined") != nullptr);
    assert(std::strstr(html, "lastSeq=d2.head") != nullptr);
    // Also verify the dashboard polls /logs on a regular interval.
    assert(std::strstr(html, "/logs?since=") != nullptr);
    std::cout << "PASS" << std::endl;
}

// ---- sendMsg yields (timeout fix) --------------------------------------
// The dashboard timeout fix: ALink::sendMsg calls portYIELD() at the
// end on Arduino so the httpd task can preempt the loop. On host,
// the function still returns control to the caller between calls —
// that's all we can prove here.
void test_sendmsg_returns_control_between_calls() {
    std::cout << "\n=== Test: sendMsg Returns Control Between Calls ===" << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    // Note: do NOT call begin() — the ALink constructor leaves the
    // link in OK so we can send without negotiating. Mirrors how
    // the other ALink tests exercise the data path in isolation.
    ALink a(mHal, true, cfg);
    assert(a.getState() == State::OK);
    // Send MAX_TX_PER_LOOP messages back-to-back (16 in the Ping
    // sketch). Each call must return to the caller. If sendMsg
    // ever blocks the loop forever, this test hangs — that's the
    // failure signal. The critical assertion is that all 16 calls
    // return true; pendingAcks count varies with chunking.
    uint8_t buf[64];
    for (int i = 0; i < 16; i++) {
        bool ok = a.sendMsg(buf, sizeof(buf));
        assert(ok);
    }
    // Sanity: at least the 16 messages' header chunks are pending
    // for ACK. The payload chunks add to this count, so >= 16.
    assert(a.pendingAcks() >= 16);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running AutoLinkWeb Tests (dashboard core, host) ===" << std::endl;
    test_stats_json_has_all_14_fields();
    test_stats_json_format();
    test_stats_json_with_pong_role();
    test_stats_json_with_empty_role();
    test_level_query_valid();
    test_level_query_rejects_none();
    test_level_query_rejects_out_of_range();
    test_level_query_rejects_empty();
    test_apply_log_level();
    test_mode_query();
    test_role_string();
    test_logs_json_empty();
    test_logs_json_with_entries();
    test_logs_json_respects_since();
    test_html_has_ping_only_class();
    test_html_has_data_role_toggle();
    test_html_has_reboot_at_top();
    test_html_has_correct_timeouts();
    test_html_skips_backlog_on_first_poll();
    test_sendmsg_returns_control_between_calls();
    std::cout << "\n=== AutoLinkWeb Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
