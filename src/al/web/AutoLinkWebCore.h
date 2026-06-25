// Dashboard core: ring log + JSON
// formatter. Host-testable; no WiFi/httpd.
#pragma once
#include "al/util/Log.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {
static constexpr int WEB_RING_CAP = 200;
static constexpr int WEB_LINE_CAP = 180;

struct WebLogEntry {
    uint32_t seq;
    char sev;
    char line[WEB_LINE_CAP];
};

struct WebSnapshot {
    char state[4];
    uint32_t errCount;
    uint32_t txBps, rxBps;
    uint64_t txTotal, rxTotal, errTotal;
    uint64_t lostMsgs;
    int32_t rssi;
    uint32_t freeHeap;
    uint32_t uptimeS;
    uint32_t baudRate;
    uint8_t fillMode;
    uint8_t msgPaused;
    uint8_t linkMode;
    int32_t txDelayMs;
    char role[8];
};

int formatStatsJson(const WebSnapshot *s, int logLevel, const char *version,
                    char *out, int outLen);

int formatLogsJson(const WebLogEntry *ring, uint32_t head, uint32_t since,
                   char *out, int outLen);

int parseLevelQuery(const char *val);

int parseModeQuery(const char *val);

bool validRoleString(const char *role);

int applyLogLevel(int lv);

const char *dashboardHtml();
int dashboardHtmlLen();
bool dashboardContains(const char *needle);

} // namespace autolink