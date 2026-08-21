
#pragma once
#include "al/util/log/Log.h"
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
    uint64_t postLockQuietDrops;
    uint64_t rateLimitedDrops;
    // E3: TX-ring stall drops (distinct from
    // rateLimitedDrops). See LinkStats.h.
    uint64_t txRingStallDrops;
    // AL90-7: log ring drops. Distinct from
    // webLogDropped (producer-side sink
    // contention, AL87-02) and from /logs
    // `dropped` (ring overrun surfaced on
    // the logs page). Pinned by
    // LogDropsSurfaceTest.
    uint64_t logDrops;
    int32_t rssi;
    uint32_t freeHeap;
    uint32_t uptimeS;
    uint32_t baudRate;
    uint8_t fillMode;
    uint8_t msgPaused;
    uint8_t linkMode;
    char linkModeLabel[8];
    int32_t txDelayMs;
    char role[8];
    // AL87-02: producer-side log-sink drops (mutex contention on
    // logSinkCb), distinct from lostMsgs (wire loss) and the
    // /logs `dropped` field (ring overrun). Surfaced here so an
    // operator watching /stats sees it even without polling
    // /logs.
    uint32_t webLogDropped;
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

struct DashboardPart {
    const char *data;
    size_t len;
};
// The seven generated parts, in wire order, for a chunked send that
// never needs the ~32 KB contiguous buffer dashboardHtml() builds.
// Returns nullptr / 0 when DASHBOARD_HTML_DEFINED wasn't set at
// compile time (the host-stub path some tests still exercise).
const DashboardPart *dashboardParts();
int dashboardPartCount();

} // namespace autolink
