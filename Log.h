#pragma once
#include <stdarg.h>
#include <stddef.h>

// --------------------------------------------------------------------------
// Log.h  —  autolink internal logger
//
// Usage:
//   Log& LOG = Log::getLog();        // singleton
//   LOG.setLevel(Log::DEBUG);
//   LOG.error("tag", "fatal %d", code);
//   LOG.info ("tag", "ready");
//   LOG.debug("tag", "value %s", "x");
//   LOG.setLevel(Log::NONE);         // silence everything
//
// Level order: NONE < INFO < DEBUG < ERROR
//   NONE  — no output at all
//   INFO  — info + error
//   DEBUG — info + debug + error
//   ERROR — error only
// --------------------------------------------------------------------------

namespace autolink {

class Log {
public:
    enum Level { NONE = 0, INFO = 1, DEBUG = 2, ERROR = 3 };

    static Log& getLog() {
        static Log inst;
        return inst;
    }

    void  setLevel(Level lv) { lvl = lv; }
    Level getLevel() const   { return lvl; }

    void error(const char* tag, const char* fmt, ...) const {
        if (lvl == NONE) return;          // only NONE suppresses errors
        va_list ap; va_start(ap, fmt);
        emit("E", tag, fmt, ap);
        va_end(ap);
    }

    void info(const char* tag, const char* fmt, ...) const {
        if (lvl < INFO || lvl == ERROR) return;
        va_list ap; va_start(ap, fmt);
        emit("I", tag, fmt, ap);
        va_end(ap);
    }

    void debug(const char* tag, const char* fmt, ...) const {
        if (lvl < DEBUG || lvl == ERROR) return;
        va_list ap; va_start(ap, fmt);
        emit("D", tag, fmt, ap);
        va_end(ap);
    }

private:
    Level lvl = INFO;

    Log() {}
    Log(const Log&)            = delete;
    Log& operator=(const Log&) = delete;

    void emit(const char* sev, const char* tag,
              const char* fmt, va_list ap) const;
};

} // namespace autolink
