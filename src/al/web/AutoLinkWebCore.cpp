
#include "al/web/AutoLinkWebCore.h"
#include "al/web/generated/DashboardPartA.h"
#include "al/web/generated/DashboardCss.h"
#include "al/web/generated/DashboardPartB.h"
#include "al/web/generated/DashboardJs1.h"
#include "al/web/generated/DashboardJs2.h"
#include "al/web/generated/DashboardJs3.h"
#include "al/web/generated/DashboardPartC.h"
#define DASHBOARD_HTML_DEFINED 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>

namespace autolink {
int formatStatsJson(const WebSnapshot *s, int logLevel, const char *version,
                    char *out, int outLen) {
    if (!s || !out || outLen <= 0)
        return 0;
    return snprintf(
        out, (size_t)outLen,
        "{\"state\":\"%s\",\"errCount\":%lu, \"errTotal\":%llu, \"lostMsgs\":%llu, \"quietDrops\":%llu, \"rateDrops\":%llu, \"txBps\":%lu,\"rxBps\":%lu, \"txTotal\":%llu,\"rxTotal\":%llu, \"rssi\":%d,\"freeHeap\":%lu,\"uptimeS\":%lu, \"baudRate\":%lu,\"lvl\":%d,\"mode\":%u, \"msgPaused\":%u, \"linkMode\":%u, \"linkModeLabel\":\"%s\", \"txDelayMs\":%d, \"role\":\"%s\", \"version\":\"%s\"}",
        s->state, (unsigned long)s->errCount, (unsigned long long)s->errTotal,
        (unsigned long long)s->lostMsgs,
        (unsigned long long)s->postLockQuietDrops,
        (unsigned long long)s->rateLimitedDrops, (unsigned long)s->txBps,
        (unsigned long)s->rxBps, (unsigned long long)s->txTotal,
        (unsigned long long)s->rxTotal, (int)s->rssi,
        (unsigned long)s->freeHeap, (unsigned long)s->uptimeS,
        (unsigned long)s->baudRate, logLevel, (unsigned)s->fillMode,
        (unsigned)s->msgPaused, (unsigned)s->linkMode, s->linkModeLabel,
        (int)s->txDelayMs, s->role, version ? version : "");
}

int formatLogsJson(const WebLogEntry *ring, uint32_t head, uint32_t since,
                   char *out, int outLen) {
    if (!ring || !out || outLen <= 0)
        return 0;
    int len = snprintf(out, (size_t)outLen, "{\"head\":%lu,\"lines\":[",
                       (unsigned long)head);
    if (len < 0 || len >= outLen)
        return len;

    uint32_t start =
        (head > (uint32_t)WEB_RING_CAP) ? (head - WEB_RING_CAP) : 0;
    if (since > start)
        start = since;

    bool first = true;
    for (uint32_t i = start; i < head; i++) {
        const WebLogEntry &e = ring[i % WEB_RING_CAP];
        if (e.seq != i)
            continue;

        if (!first) {
            int n = snprintf(out + len, (size_t)(outLen - len), ",");
            if (n < 0 || n >= outLen - len)
                break;
            len += n;
        }
        first = false;

        int n = snprintf(out + len, (size_t)(outLen - len),
                         "{\"seq\":%lu,\"sev\":\"%c\",\"text\":\"",
                         (unsigned long)i, e.sev);
        if (n < 0 || n >= outLen - len)
            break;
        len += n;

        for (const char *p = e.line; *p && len < outLen - 4; p++) {
            switch (*p) {
            case '"':
                out[len++] = '\\';
                out[len++] = '"';
                break;
            case '\\':
                out[len++] = '\\';
                out[len++] = '\\';
                break;
            case '\n':
                out[len++] = '\\';
                out[len++] = 'n';
                break;
            case '\r':
                out[len++] = '\\';
                out[len++] = 'r';
                break;
            default:
                out[len++] = *p;
                break;
            }
        }
        if (len < outLen - 2) {
            out[len++] = '"';
            out[len++] = '}';
        }
    }

    int n = snprintf(out + len, (size_t)(outLen - len), "]}");
    if (n > 0 && n < outLen - len)
        len += n;
    return len;
}

int parseLevelQuery(const char *val) {
    if (!val || !*val)
        return -1;
    int lv = atoi(val);
    if (lv == (int)Log::NONE)
        return -2;
    if (lv < 0 || lv > (int)Log::VERBOSE)
        return -3;
    return lv;
}

int parseModeQuery(const char *val) {
    if (!val)
        return -1;
    if (strcmp(val, "seq") == 0)
        return 0;
    if (strcmp(val, "rand") == 0)
        return 1;
    return -1;
}

bool validRoleString(const char *role) {
    if (!role)
        return false;
    size_t n = strlen(role);

    return n > 0 && n < 8;
}

int applyLogLevel(int lv) {
    if (lv == (int)Log::NONE)
        return -1;
    if (lv < 0 || lv > (int)Log::VERBOSE)
        return -2;
    Log::log().setLevel((Log::Level)lv);
    return lv;
}

#ifdef DASHBOARD_HTML_DEFINED
static const DashboardPart kParts[] = {
    { DASHBOARD_HTML_PART_A, sizeof(DASHBOARD_HTML_PART_A) - 1 },
    { DASHBOARD_CSS, sizeof(DASHBOARD_CSS) - 1 },
    { DASHBOARD_HTML_PART_B, sizeof(DASHBOARD_HTML_PART_B) - 1 },
    { DASHBOARD_JS_1, sizeof(DASHBOARD_JS_1) - 1 },
    { DASHBOARD_JS_2, sizeof(DASHBOARD_JS_2) - 1 },
    { DASHBOARD_JS_3, sizeof(DASHBOARD_JS_3) - 1 },
    { DASHBOARD_HTML_PART_C, sizeof(DASHBOARD_HTML_PART_C) - 1 },
};
const DashboardPart *dashboardParts() { return kParts; }
int dashboardPartCount() { return (int)(sizeof(kParts) / sizeof(kParts[0])); }

namespace {
const std::string &assembledDashboard() {
    // Built once, on first call, by concatenating the seven generated
    // parts in wire order. Nothing on the ESP32 request path calls
    // this -- AutoLinkWeb::handleRoot chunk-sends the seven parts
    // directly, so this ~32 KB allocation only happens for a caller
    // that genuinely needs one contiguous buffer (host tests today).
    static std::string html;
    if (html.empty()) {
        html.reserve(sizeof(DASHBOARD_HTML_PART_A) + sizeof(DASHBOARD_CSS) +
                     sizeof(DASHBOARD_HTML_PART_B) + sizeof(DASHBOARD_JS_1) +
                     sizeof(DASHBOARD_JS_2) + sizeof(DASHBOARD_JS_3) +
                     sizeof(DASHBOARD_HTML_PART_C));
        html += DASHBOARD_HTML_PART_A;
        html += DASHBOARD_CSS;
        html += DASHBOARD_HTML_PART_B;
        html += DASHBOARD_JS_1;
        html += DASHBOARD_JS_2;
        html += DASHBOARD_JS_3;
        html += DASHBOARD_HTML_PART_C;
    }
    return html;
}
} // namespace
const char *dashboardHtml() { return assembledDashboard().c_str(); }
int dashboardHtmlLen() { return (int)assembledDashboard().size(); }
bool dashboardContains(const char *needle) {
    return needle && strstr(dashboardHtml(), needle) != nullptr;
}
#else
static const char HTML_STUB[] = "";
const char *dashboardHtml() { return HTML_STUB; }
int dashboardHtmlLen() { return 0; }
bool dashboardContains(const char *) { return false; }
const DashboardPart *dashboardParts() { return nullptr; }
int dashboardPartCount() { return 0; }
#endif

} // namespace autolink
