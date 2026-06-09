#include "Log.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

// --------------------------------------------------------------------------
// Log.cpp  —  autolink internal logger implementation
//
// On ESP_PLATFORM the formatted line is passed to the matching ESP_LOG*
// macro so it still appears in the IDF monitor stream, 
// On host builds the line goes straight to stdout.
// --------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#  include "esp_log.h"
#endif

namespace autolink {
/*
static void hhmmss(char* buf, size_t n) {
    time_t t; time(&t);
    struct tm* tm_ = localtime(&t);
    if (tm_) strftime(buf, n, "%H:%M:%S", tm_);
    else      strncpy(buf, "00:00:00", n);
}
*/
void Log::emit(const char* sev, const char* tag,
               const char* fmt, va_list ap) const
{
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    //char tbuf[10];
    //hhmmss(tbuf, sizeof(tbuf));

    // Full line: [HH:MM:SS] [SEV] [tag] message
    //char line[320];
    //snprintf(line, sizeof(line), "[%s] [%s] [%s] %s", tbuf, sev, tag, msg);

#ifdef ESP_PLATFORM
    switch (sev[0]) {
        case 'E': ESP_LOGE(tag, "%s", msg); break;
        case 'D': ESP_LOGD(tag, "%s", msg); break;
        default:  ESP_LOGI(tag, "%s", msg); break;
    }
#else
    puts(line);
#endif
}

} // namespace autolink
