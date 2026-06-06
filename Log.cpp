#include "Log.h"
#include <stdio.h>

// --------------------------------------------------------------------------
// Log.cpp  —  autolink internal logger implementation
//
// On ESP_PLATFORM: routes through esp_log so output goes to the serial
// console the same way as the rest of the firmware.
// On host / unit-test builds: writes to stdout with a timestamp prefix.
// All output is suppressed when level == NONE; no Stream.print* calls are
// made at all in that case (the guard is in Log.h before emit() is called).
// --------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#else
#  include <time.h>
#  include <string.h>
#endif

namespace autolink {

void Log::emit(const char* sev, const char* tag,
               const char* fmt, va_list ap) const
{
    // Build the formatted message into a fixed buffer.
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);

#ifdef ESP_PLATFORM
    // Delegate to ESP-IDF logging; honours its own level filter too.
    if (sev[0] == 'D') {
        ESP_LOGD(tag, "%s", msg);
    } else {
        ESP_LOGI(tag, "%s", msg);
    }
#else
    // Host build — write to stdout with wall-clock prefix.
    char tbuf[10] = "00:00:00";
    time_t t; time(&t);
    struct tm* tm_ = localtime(&t);
    if (tm_) strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_);

    // Gate: only reach here when lvl >= INFO (or DEBUG); NONE never calls
    // emit() at all — so no Stream.print* equivalent is invoked.
    printf("[%s] [%s] [%s] %s\n", tbuf, sev, tag, msg);
#endif
}

} // namespace autolink
