// AutoLinkWebCore.cpp — host-testable dashboard core.
//
// Pure C++ implementation of the dashboard's observable behavior:
// JSON formatting, query parsing, level validation, role checks, and
// a read-only accessor for the embedded HTML. No Arduino, no
// FreeRTOS, no esp_http_server, no NVS.
//
// The actual HTML/CSS/JS string lives at the bottom of this file
// (also referenced by AutoLinkWeb.cpp::handleRoot for the Arduino
// build). Host tests use dashboardContains() to verify class names
// and structure without parsing HTML.
#include "al/web/AutoLinkWebCore.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace autolink {

// ---- formatStatsJson ----------------------------------------------------
int formatStatsJson(const WebSnapshot* s,
                    int logLevel,
                    const char* version,
                    char* out, int outLen) {
    if (!s || !out || outLen <= 0) return 0;
    return snprintf(out, (size_t)outLen,
        "{\"state\":\"%s\",\"errCount\":%lu,\"errTotal\":%llu,"
        "\"lostMsgs\":%llu,"
        "\"txBps\":%lu,\"rxBps\":%lu,"
        "\"txTotal\":%llu,\"rxTotal\":%llu,"
        "\"rssi\":%d,\"freeHeap\":%lu,\"uptimeS\":%lu,"
        "\"baudRate\":%lu,\"lvl\":%d,\"mode\":%u,"
        "\"role\":\"%s\","
        "\"version\":\"%s\"}",
        s->state,
        (unsigned long)s->errCount,
        (unsigned long long)s->errTotal,
        (unsigned long long)s->lostMsgs,
        (unsigned long)s->txBps,
        (unsigned long)s->rxBps,
        (unsigned long long)s->txTotal,
        (unsigned long long)s->rxTotal,
        (int)s->rssi,
        (unsigned long)s->freeHeap,
        (unsigned long)s->uptimeS,
        (unsigned long)s->baudRate,
        logLevel,
        (unsigned)s->fillMode,
        s->role,
        version ? version : "");
}

// ---- formatLogsJson -----------------------------------------------------
int formatLogsJson(const WebLogEntry* ring,
                   uint32_t head,
                   uint32_t since,
                   char* out, int outLen) {
    if (!ring || !out || outLen <= 0) return 0;
    // First chunk: include current `head` so the client can skip the
    // boot-time backlog on first poll. JS reads d2.head and sets
    // lastSeq = head; subsequent polls get only entries that arrive
    // after page load.
    int len = snprintf(out, (size_t)outLen, "{\"head\":%lu,\"lines\":[",
                       (unsigned long)head);
    if (len < 0 || len >= outLen) return len;

    uint32_t start = (head > (uint32_t)WEB_RING_CAP)
                         ? (head - WEB_RING_CAP) : 0;
    if (since > start) start = since;

    bool first = true;
    for (uint32_t i = start; i < head; i++) {
        const WebLogEntry& e = ring[i % WEB_RING_CAP];
        if (e.seq != i) continue; // defensive: slot was overwritten

        if (!first) {
            int n = snprintf(out + len, (size_t)(outLen - len), ",");
            if (n < 0 || n >= outLen - len) break;
            len += n;
        }
        first = false;

        int n = snprintf(out + len, (size_t)(outLen - len),
                         "{\"seq\":%lu,\"sev\":\"%c\",\"text\":\"",
                         (unsigned long)i, e.sev);
        if (n < 0 || n >= outLen - len) break;
        len += n;

        // JSON-escape the log line. The line is already NUL-terminated
        // by the writer (snprintf into a fixed buffer).
        for (const char* p = e.line; *p && len < outLen - 4; p++) {
            switch (*p) {
                case '"':  out[len++] = '\\'; out[len++] = '"';  break;
                case '\\': out[len++] = '\\'; out[len++] = '\\'; break;
                case '\n': out[len++] = '\\'; out[len++] = 'n';  break;
                case '\r': out[len++] = '\\'; out[len++] = 'r';  break;
                default:   out[len++] = *p;                     break;
            }
        }
        if (len < outLen - 2) { out[len++] = '"'; out[len++] = '}'; }
    }

    int n = snprintf(out + len, (size_t)(outLen - len), "]}");
    if (n > 0 && n < outLen - len) len += n;
    return len;
}

// ---- query parsing ------------------------------------------------------
int parseLevelQuery(const char* val) {
    if (!val || !*val) return -1;
    int lv = atoi(val);
    if (lv == (int)Log::NONE) return -2;  // rejected
    if (lv < 0 || lv > (int)Log::VERBOSE) return -3;
    return lv;
}

int parseModeQuery(const char* val) {
    if (!val) return -1;
    if (strcmp(val, "seq") == 0)  return 0;
    if (strcmp(val, "rand") == 0) return 1;
    return -1;
}

bool validRoleString(const char* role) {
    if (!role) return false;
    size_t n = strlen(role);
    // role[8] in WebSnapshot — must leave room for NUL.
    return n > 0 && n < 8;
}

int applyLogLevel(int lv) {
    if (lv == (int)Log::NONE)            return -1; // rejected
    if (lv < 0 || lv > (int)Log::VERBOSE) return -2; // out of range
    Log::log().setLevel((Log::Level)lv);
    return lv;
}

// ---- dashboard HTML -----------------------------------------------------
//
// The HTML is defined as a single raw string literal. Host tests use
// dashboardContains() to verify class names, IDs, and conditional UI
// elements without parsing HTML. The same string is embedded in
// AutoLinkWeb.cpp via #include "web/AutoLinkWebCore.cpp" at the bottom
// of that file (so there's a single source of truth — the dashboard
// HTML is not duplicated).
//
// Keeping the HTML in a .cpp file (not a .h) lets host tests link
// AutoLinkWebCore.cpp without dragging in <Arduino.h> or other
// platform-specific includes. The full HTML is too long to inline
// here in a useful way, so we keep it in a separate string.
//
// IMPORTANT: this is a no-op stub for the test surface. The real
// dashboard HTML lives in AutoLinkWeb.cpp::HTML_PAGE[] and is
// included via the macro DASHBOARD_HTML_DEFINED below when the
// Arduino build pulls AutoLinkWeb.cpp.
#ifndef DASHBOARD_HTML_DEFINED
static const char HTML_STUB[] = "";
const char* dashboardHtml() { return HTML_STUB; }
int         dashboardHtmlLen() { return 0; }
bool dashboardContains(const char*) { return false; }
#endif

} // namespace autolink
