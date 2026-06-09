#include "Log.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

// --------------------------------------------------------------------------
// Log.cpp  —  autolink internal logger implementation
//
// Both platforms format the message as:
//   [HH:MM:SS] [SEV] [tag] message
//
// On ESP_PLATFORM the formatted line is passed to the matching ESP_LOG*
// macro so it still appears in the IDF monitor stream, with the hhmmss
// prefix baked in regardless of the monitor's own timestamp setting.
//
// On host builds the line goes straight to stdout.
//
// NONE suppresses all output.  ERROR level passes only error() calls.
// --------------------------------------------------------------------------

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#endif

namespace autolink {

static void hhmmss(char* buf, size_t n) {
    time_t t; time(&t);
    struct tm* tm_ = localtime(&t);
    if (tm_) strftime(buf, n, "%H:%M:%S", tm_);
    else      strncpy(buf, "00:00:00", n);
}

void Log::emit(const char* sev, const char* tag,
               const char* fmt, va_list ap) const
{
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    char tbuf[10];
    hhmmss(tbuf, sizeof(tbuf));

    // Full line: [HH:MM:SS] [SEV] [tag] message
    char line[320];
    snprintf(line, sizeof(line), "[%s] [%s] [%s] %s", tbuf, sev, tag, msg);

#ifdef ESP_PLATFORM
    switch (sev[0]) {
        case 'E': ESP_LOGE(tag, "%s", line); break;
        case 'D': Serial.printf("%s\n", line); break;  // bypass ESP_LOGD
        default:  Serial.printf("%s\n", line); break;  // bypass ESP_LOGI

    //    case 'D': ESP_LOGD(tag, "%s", line); break;
    //    default:  ESP_LOGI(tag, "%s", line); break;
    }
#else
    puts(line);
#endif
}

} // namespace autolink
