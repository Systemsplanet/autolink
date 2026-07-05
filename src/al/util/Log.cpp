// Logger emit: ESP_LOG* on device,
// stdout on host, optional sink cb.
#include "al/util/Log.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#    define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#    include "esp_log.h"
#endif

namespace autolink {
void Log::emit(const char *sev, const char *tag, const char *fmt,
               va_list ap) const {
    if (lvl == NONE)
        return;

    char msg[384];
    int needed = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (needed > (int)sizeof(msg) - 1) {
        static bool truncWarned_ = false;
        if (!truncWarned_) {
            fprintf(
                stderr,
                "E [%s] Log::emit: line truncated (needed %d bytes, buffer %u). Shorten the format string or raise the buffer size.\n",
                tag, needed, (unsigned)sizeof(msg));
            truncWarned_ = true;
        }
    }

#ifdef ESP_PLATFORM
    switch (sev[0]) {
    case 'E':
        ESP_LOGE(tag, "%s", msg);
        break;
    case 'W':
        ESP_LOGW(tag, "%s", msg);
        break;
    case 'V':
        ESP_LOGV(tag, "%s", msg);
        break;
    case 'D':
        ESP_LOGD(tag, "%s", msg);
        break;
    default:
        ESP_LOGI(tag, "%s", msg);
        break;
    }
#else
    fprintf(stdout, "%c [%s] %s\n", sev[0], tag, msg);
    fflush(stdout);
#endif

    if (sink_fn_)
        sink_fn_(sev[0], tag, msg, sink_ctx_);
}

void Log::setSink(LogSink fn, void *ctx) {
    sink_fn_ = fn;
    sink_ctx_ = ctx;
}

void Log::clearSink() {
    sink_fn_ = nullptr;
    sink_ctx_ = nullptr;
}

} // namespace autolink