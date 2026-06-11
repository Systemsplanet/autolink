#pragma once
#include <stdarg.h>
#include <stddef.h>

// --------------------------------------------------------------------------
// Log — leveled singleton logger for the AutoLink stack: ESP_LOG* on device,
// stdout on host builds.
//
// Usage:
//   Log& LOG = Log::getLog();        // singleton
//   LOG.setLevel(Log::DEBUG);
//   LOG.error("class", "fatal %d", code);
//   LOG.info ("class", "ready");
//   LOG.debug("class", "value %s", "x");
//   LOG.setLevel(Log::ERROR);         
//
// Level order: ERROR < INFO < DEBUG 
//
//   ERROR — error only
//   INFO  — error + info
//   DEBUG — error + info + debug

// --------------------------------------------------------------------------

namespace autolink {

class Log {
public:
    enum Level { ERROR = 0, INFO = 1, DEBUG = 2};

    static Log& getLog() {
        static Log inst;
        return inst;
    }

    void  setLevel(Level lv) { lvl = lv; }
    Level getLevel() const   { return lvl; }

    void error(const char* tag, const char* fmt, ...) const {
        va_list ap; va_start(ap, fmt);
        emit("E", tag, fmt, ap);
        va_end(ap);
    }

    void info(const char* tag, const char* fmt, ...) const {
        if (lvl == ERROR) return;
        va_list ap; va_start(ap, fmt);
        emit("I", tag, fmt, ap);
        va_end(ap);
    }

    void debug(const char* tag, const char* fmt, ...) const {
        if (lvl == INFO || lvl == ERROR) return;
        va_list ap; va_start(ap, fmt);
        emit("D", tag, fmt, ap);
        va_end(ap);
    }

    // ---- Optional output sink ----
    // A single callback registered once (e.g. by AutoLinkWeb) to receive
    // every formatted log line after the normal ESP_LOG / stdout output.
    // Must be fast and non-blocking: it runs in whatever task calls emit().
    // setSink() is safe to call from setup() before UART tasks start.
    using LogSink = void(*)(char sev, const char* tag, const char* msg, void* ctx);
    void setSink(LogSink fn, void* ctx = nullptr);
    void clearSink();

private:
    Level lvl = ERROR;

    // Mutable so emit() (const) can call the sink without a const_cast.
    mutable LogSink  sink_fn_  = nullptr;
    mutable void*    sink_ctx_ = nullptr;

    Log() {}
    Log(const Log&)            = delete;
    Log& operator=(const Log&) = delete;

    void emit(const char* sev, const char* tag,
              const char* fmt, va_list ap) const;
};

} // namespace autolink
