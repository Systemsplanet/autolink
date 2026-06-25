// Dashboard JSON + log ring + level/mode.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <string>
#    include "al/util/Log.h"
#    include "al/link/Link.h"
#    include "AutoLink.h"
#    include "al/web/AutoLinkWebCore.h"
#    include "al/web/AutoLinkWebHtml.h"
#    include "MockHal.h"

using namespace autolink;

void test_stats_json_has_all_14_fields()
{
    std::cout << "\n=== Test: /stats JSON has all 14 WebSnapshot fields ==="
              << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    s.errCount = 1;
    s.txBps = 11;
    s.rxBps = 22;
    s.txTotal = 100;
    s.rxTotal = 200;
    s.errTotal = 3;
    s.lostMsgs = 4;
    s.rssi = -50;
    s.freeHeap = 99999;
    s.uptimeS = 60;
    s.baudRate = 115200;
    s.fillMode = 1;
    std::strcpy(s.role, "Ping");
    char buf[512];
    int len = formatStatsJson(&s, 3, "1.0.0", buf, sizeof(buf));
    assert(len > 0);

    const char *mustHave[] = {
        "\"state\":",   "\"errCount\":", "\"txBps\":",    "\"rxBps\":",
        "\"txTotal\":", "\"rxTotal\":",  "\"errTotal\":", "\"lostMsgs\":",
        "\"rssi\":",    "\"freeHeap\":", "\"uptimeS\":",  "\"baudRate\":",
        "\"lvl\":",     "\"mode\":",     "\"role\":",     "\"version\":",
        nullptr
    };
    for (int i = 0; mustHave[i]; i++) {
        assert(std::strstr(buf, mustHave[i]) != nullptr);
    }

    assert(std::strstr(buf, "\"mode\":1") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_stats_json_format()
{
    std::cout << "\n=== Test: /stats JSON Format ===" << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    s.errCount = 42;
    s.txBps = 1024;
    s.rxBps = 2048;
    s.txTotal = 1000000;
    s.rxTotal = 2000000;
    s.errTotal = 5;
    s.lostMsgs = 0;
    s.rssi = -65;
    s.freeHeap = 200000;
    s.uptimeS = 3600;
    s.baudRate = 115200;
    s.fillMode = 0;
    std::strcpy(s.role, "Ping");

    char buf[512];
    int len = formatStatsJson(&s, 3, "5.0.7", buf, sizeof(buf));
    assert(len > 0);
    assert(len < (int)sizeof(buf));

    assert(std::strstr(buf, "\"state\":\"OK\"") != nullptr);
    assert(std::strstr(buf, "\"errCount\":42") != nullptr);
    assert(std::strstr(buf, "\"txBps\":1024") != nullptr);
    assert(std::strstr(buf, "\"rxBps\":2048") != nullptr);
    assert(std::strstr(buf, "\"txTotal\":1000000") != nullptr);
    assert(std::strstr(buf, "\"rxTotal\":2000000") != nullptr);
    assert(std::strstr(buf, "\"errTotal\":5") != nullptr);
    assert(std::strstr(buf, "\"rssi\":-65") != nullptr);
    assert(std::strstr(buf, "\"freeHeap\":200000") != nullptr);
    assert(std::strstr(buf, "\"uptimeS\":3600") != nullptr);
    assert(std::strstr(buf, "\"baudRate\":115200") != nullptr);
    assert(std::strstr(buf, "\"lvl\":3") != nullptr);
    assert(std::strstr(buf, "\"mode\":0") != nullptr);
    assert(std::strstr(buf, "\"role\":\"Ping\"") != nullptr);
    assert(std::strstr(buf, "\"version\":\"5.0.7\"") != nullptr);

    assert(buf[0] == '{');
    assert(buf[len - 1] == '}');
    std::cout << "PASS" << std::endl;
}

void test_stats_json_with_pong_role()
{
    std::cout << "\n=== Test: /stats JSON Includes 'Pong' Role ==="
              << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "OK");
    std::strcpy(s.role, "Pong");
    char buf[512];
    int len = formatStatsJson(&s, 3, "5.0.7", buf, sizeof(buf));
    (void)len;
    assert(std::strstr(buf, "\"role\":\"Pong\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_stats_json_with_empty_role()
{
    std::cout << "\n=== Test: /stats JSON With Empty Role (Legacy Code) ==="
              << std::endl;
    WebSnapshot s = {};
    std::strcpy(s.state, "SWP");
    s.role[0] = '\0';
    char buf[512];
    int len = formatStatsJson(&s, 4, "5.0.7", buf, sizeof(buf));
    (void)len;
    assert(std::strstr(buf, "\"role\":\"\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_level_query_valid()
{
    std::cout << "\n=== Test: parseLevelQuery Valid Levels ===" << std::endl;
    assert(parseLevelQuery("1") == 1);
    assert(parseLevelQuery("2") == 2);
    assert(parseLevelQuery("3") == 3);
    assert(parseLevelQuery("4") == 4);
    assert(parseLevelQuery("5") == 5);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_none()
{
    std::cout << "\n=== Test: parseLevelQuery Rejects lv=0 (NONE) ==="
              << std::endl;

    assert(parseLevelQuery("0") == -2);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_out_of_range()
{
    std::cout << "\n=== Test: parseLevelQuery Rejects Out-of-Range ==="
              << std::endl;

    assert(parseLevelQuery("6") == -3);
    assert(parseLevelQuery("99") == -3);

    assert(parseLevelQuery("-1") == -3);

    assert(parseLevelQuery("abc") == -2);
    std::cout << "PASS" << std::endl;
}

void test_level_query_rejects_empty()
{
    std::cout << "\n=== Test: parseLevelQuery Rejects Empty/Missing ==="
              << std::endl;
    assert(parseLevelQuery("") == -1);
    assert(parseLevelQuery(nullptr) == -1);
    std::cout << "PASS" << std::endl;
}

void test_apply_log_level()
{
    std::cout << "\n=== Test: applyLogLevel Mutates Log Singleton ==="
              << std::endl;
    Log &L = Log::log();
    L.setLevel(Log::INFO);
    int rc = applyLogLevel(Log::DEBUG);
    assert(rc == (int)Log::DEBUG);
    assert(L.getLevel() == Log::DEBUG);

    rc = applyLogLevel(Log::NONE);
    assert(rc == -1);
    assert(L.getLevel() == Log::DEBUG);

    rc = applyLogLevel(99);
    assert(rc == -2);
    assert(L.getLevel() == Log::DEBUG);
    std::cout << "PASS" << std::endl;
}

void test_mode_query()
{
    std::cout << "\n=== Test: parseModeQuery ===" << std::endl;
    assert(parseModeQuery("seq") == 0);
    assert(parseModeQuery("rand") == 1);
    assert(parseModeQuery("SEQ") == -1);
    assert(parseModeQuery("") == -1);
    assert(parseModeQuery(nullptr) == -1);
    std::cout << "PASS" << std::endl;
}

void test_role_string()
{
    std::cout << "\n=== Test: validRoleString ===" << std::endl;

    assert(validRoleString("Ping") == true);
    assert(validRoleString("Pong") == true);
    assert(validRoleString("") == false);
    assert(validRoleString(nullptr) == false);
    assert(validRoleString("ABCDEF") == true);
    assert(validRoleString("ABCDEFG") == true);
    assert(validRoleString("ABCDEFGH") == false);
    std::cout << "PASS" << std::endl;
}

void test_logs_json_empty()
{
    std::cout << "\n=== Test: formatLogsJson Empty Ring ===" << std::endl;
    WebLogEntry ring[10] = {};
    char buf[256];
    int len = formatLogsJson(ring, 0, 0, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":0") != nullptr);
    assert(std::strstr(buf, "\"lines\":[]") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_logs_json_with_entries()
{
    std::cout << "\n=== Test: formatLogsJson With Entries ===" << std::endl;
    WebLogEntry ring[10] = {};
    ring[0].seq = 0;
    ring[0].sev = 'I';
    std::strcpy(ring[0].line, "12:34:56.789 I ALinkWeb hello");
    ring[1].seq = 1;
    ring[1].sev = 'W';
    std::strcpy(ring[1].line, "12:34:57.000 W ALinkWeb warning");
    ring[2].seq = 2;
    ring[2].sev = 'E';
    std::strcpy(ring[2].line, "12:34:58.000 E ALinkWeb \"quoted\" msg");

    char buf[2048];
    int len = formatLogsJson(ring, 3, 0, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":3") != nullptr);
    assert(std::strstr(buf, "\"seq\":0") != nullptr);
    assert(std::strstr(buf, "\"seq\":1") != nullptr);
    assert(std::strstr(buf, "\"seq\":2") != nullptr);
    assert(std::strstr(buf, "\"sev\":\"I\"") != nullptr);
    assert(std::strstr(buf, "\"sev\":\"W\"") != nullptr);
    assert(std::strstr(buf, "\"sev\":\"E\"") != nullptr);

    assert(std::strstr(buf, "\\\"quoted\\\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_logs_json_respects_since()
{
    std::cout << "\n=== Test: formatLogsJson Respects since ===" << std::endl;
    WebLogEntry ring[10] = {};
    ring[0].seq = 0;
    ring[0].sev = 'I';
    std::strcpy(ring[0].line, "first");
    ring[1].seq = 1;
    ring[1].sev = 'I';
    std::strcpy(ring[1].line, "second");
    ring[2].seq = 2;
    ring[2].sev = 'I';
    std::strcpy(ring[2].line, "third");

    char buf[2048];

    int len = formatLogsJson(ring, 3, 1, buf, sizeof(buf));
    assert(len > 0);
    assert(std::strstr(buf, "\"head\":3") != nullptr);
    assert(std::strstr(buf, "\"first\"") == nullptr);
    assert(std::strstr(buf, "\"second\"") != nullptr);
    assert(std::strstr(buf, "\"third\"") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_ping_only_class()
{
    std::cout
        << "\n=== Test: HTML has .ping-only class on Ping-only controls (not on log-pause) ==="
        << std::endl;
    const char *html = DASHBOARD_HTML;

    assert(std::strstr(html, "id=\"modeGroup\"") != nullptr);
    assert(
        std::strstr(html, "class=\"lvl-group ping-only\" id=\"modeGroup\"") !=
        nullptr);

    assert(std::strstr(html, "id=\"topPbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn pause ping-only\" id=\"topPbtn\"") !=
           nullptr);

    assert(std::strstr(html, "id=\"pbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn\" id=\"pbtn\"") != nullptr);
    assert(std::strstr(html, "class=\"btn ping-only\" id=\"pbtn\"") == nullptr);

    assert(std::strstr(html, "id=\"pbtn2\"") != nullptr);
    assert(std::strstr(html, "class=\"btn\" id=\"pbtn2\"") != nullptr);
    assert(std::strstr(html, "class=\"btn ping-only\" id=\"pbtn2\"") ==
           nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_data_role_toggle()
{
    std::cout << "\n=== Test: HTML has CSS to hide .ping-only on Pong ==="
              << std::endl;
    const char *html = DASHBOARD_HTML;

    assert(std::strstr(html, "body[data-role=\"pong\"] .ping-only") != nullptr);

    assert(
        std::strstr(html, "document.body.setAttribute('data-role','ping')") !=
        nullptr);
    assert(
        std::strstr(html, "document.body.setAttribute('data-role','pong')") !=
        nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_reboot_at_top()
{
    std::cout << "\n=== Test: Reboot Button is in Header (Top of GUI) ==="
              << std::endl;
    const char *html = DASHBOARD_HTML;

    const char *rebootBtn = std::strstr(html, "id=\"rebootBtnTop\"");
    assert(rebootBtn != nullptr);
    const char *headerEnd = std::strstr(html, "</header>");
    assert(headerEnd != nullptr);
    assert(rebootBtn < headerEnd);

    assert(std::strstr(html, "id=\"rebootBtn\"") == nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_has_correct_timeouts()
{
    std::cout << "\n=== Test: JS fetch timeout is 5s (not 2.5s) ==="
              << std::endl;
    const char *html = DASHBOARD_HTML;

    assert(std::strstr(html, "ms||5000") != nullptr);

    assert(std::strstr(html, "2500") == nullptr);
    std::cout << "PASS" << std::endl;
}

void test_html_skips_backlog_on_first_poll()
{
    std::cout << "\n=== Test: JS Skips Backlog on First Poll ===" << std::endl;
    const char *html = DASHBOARD_HTML;

    assert(std::strstr(html, "d2.head!==undefined") != nullptr);
    assert(std::strstr(html, "lastSeq=d2.head") != nullptr);

    assert(std::strstr(html, "/logs?since=") != nullptr);
    std::cout << "PASS" << std::endl;
}

void test_sendmsg_returns_control_between_calls()
{
    std::cout << "\n=== Test: sendMsg Returns Control Between Calls ==="
              << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;

    Link a(mHal, true, cfg);
    assert(a.getState() == State::OK);

    uint8_t buf[64];
    for (int i = 0; i < 16; i++) {
        bool ok = a.sendMsg(buf, sizeof(buf));
        assert(ok);
    }

    assert(a.pendingAcks() >= 16);
    std::cout << "PASS" << std::endl;
}

void test_reset_zeros_all_dashboard_counters()
{
    std::cout
        << "\n=== Test: Reset zeros all dashboard counters (regression) ==="
        << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, true, cfg);
    assert(a.getState() == State::OK);

    uint8_t buf[128];
    for (int i = 0; i < 5; i++)
        assert(a.sendMsg(buf, sizeof(buf)));

    for (int i = 0; i < 3; i++)
        a.err();

    Stats sBefore;
    a.getStats(sBefore);
    assert(sBefore.tx > 0 || sBefore.frameErrs > 0 || sBefore.discCount == 0);

    assert(sBefore.frameErrs == 3);

    a.resetStats();
    a.resetErrors();
    a.resetDiag();

    Stats sAfter;
    a.getStats(sAfter);
    if (sAfter.tx != 0) {
        std::cerr << "\ntx should be 0 after reset (was "
                  << (long long)sAfter.tx << ")" << std::endl;
    }
    assert(sAfter.tx == 0);
    if (sAfter.rx != 0) {
        std::cerr << "\nrx should be 0 after reset (was "
                  << (long long)sAfter.rx << ")" << std::endl;
    }
    assert(sAfter.rx == 0);
    if (sAfter.discCount != 0) {
        std::cerr << "\ndiscCount should be 0 after reset (was "
                  << (long long)sAfter.discCount << ")" << std::endl;
    }
    assert(sAfter.discCount == 0);
    if (sAfter.frameErrs != 0) {
        std::cerr << "\nframeErrs should be 0 after reset (was "
                  << (long long)sAfter.frameErrs << ")" << std::endl;
    }
    assert(sAfter.frameErrs == 0);

    WebSnapshot snap = {};
    std::strcpy(snap.state, "OK");
    snap.txTotal = sAfter.tx;
    snap.rxTotal = sAfter.rx;
    snap.errTotal = (uint32_t)sAfter.discCount;
    snap.errCount = (uint32_t)sAfter.frameErrs;
    char json[512];
    int n = formatStatsJson(&snap, 3, "1.0.0", json, sizeof(json));
    assert(n > 0 && n < (int)sizeof(json));

    std::string jstr(json);
    if (jstr.find("\"txTotal\":0") == std::string::npos) {
        std::cerr << "\ntxTotal should be 0 in /stats JSON, got: " << jstr
                  << std::endl;
    }
    assert(jstr.find("\"txTotal\":0") != std::string::npos);
    if (jstr.find("\"rxTotal\":0") == std::string::npos) {
        std::cerr << "\nrxTotal should be 0 in /stats JSON, got: " << jstr
                  << std::endl;
    }
    assert(jstr.find("\"rxTotal\":0") != std::string::npos);
    if (jstr.find("\"errTotal\":0") == std::string::npos) {
        std::cerr << "\nerrTotal should be 0 in /stats JSON, got: " << jstr
                  << std::endl;
    }
    assert(jstr.find("\"errTotal\":0") != std::string::npos);
    if (jstr.find("\"errCount\":0") == std::string::npos) {
        std::cerr << "\nerrCount should be 0 in /stats JSON, got: " << jstr
                  << std::endl;
    }
    assert(jstr.find("\"errCount\":0") != std::string::npos);
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout << "=== Running AutoLinkWeb Tests (dashboard core, host) ==="
              << std::endl;
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
    test_reset_zeros_all_dashboard_counters();
    std::cout << "\n=== AutoLinkWeb Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif