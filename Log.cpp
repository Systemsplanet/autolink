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

void Log::emit(const char* sev, const char* tag,
               const char* fmt, va_list ap) const
{
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);

#ifdef ESP_PLATFORM
    switch (sev[0]) {
        case 'E': ESP_LOGE(tag, "%s", msg); break;
        case 'D': ESP_LOGD(tag, "%s", msg); break;
        default:  ESP_LOGI(tag, "%s", msg); break;
    }
#else
  //todo: format line line esp32 logger
    puts(line);
#endif
}

} // namespace autolink
