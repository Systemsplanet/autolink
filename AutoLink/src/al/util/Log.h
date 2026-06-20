#pragma once
#include <stdarg.h>
#include <stddef.h>

// Log — leveled singleton logger.
// On ESP_PLATFORM: routes to ESP_LOGE/W/I/D/V. On host: stdout.
namespace autolink {

class Log {
public:
    // Emit rule: level_for_msg <= current_level.
    enum Level {
        NONE    = 0,
        ERROR   = 1,
        WARNING = 2,
        INFO    = 3,
        DEBUG   = 4,
        VERBOSE = 5
    };

    static Log& log() {
        static Log inst;
        return inst;
    }

    void  setLevel(Level lv) { lvl = lv; }
    Level getLevel() const   { return lvl; }

    bool wouldEmit(Level lvl_for_msg) const {
        return lvl_for_msg <= lvl;
    }

    void error(const char* tag, const char* fmt, ...) const {
        if (lvl < ERROR) return;
        va_list ap; va_start(ap, fmt);
        emit("E", tag, fmt, ap);
        va_end(ap);
    }

    void warning(const char* tag, const char* fmt, ...) const {
        if (lvl < WARNING) return;
        va_list ap; va_start(ap, fmt);
        emit("W", tag, fmt, ap);
        va_end(ap);
    }

    void info(const char* tag, const char* fmt, ...) const {
        if (lvl < INFO) return;
        va_list ap; va_start(ap, fmt);
        emit("I", tag, fmt, ap);
        va_end(ap);
    }

    void debug(const char* tag, const char* fmt, ...) const {
        if (lvl < DEBUG) return;
        va_list ap; va_start(ap, fmt);
        emit("D", tag, fmt, ap);
        va_end(ap);
    }

    void verbose(const char* tag, const char* fmt, ...) const {
        if (lvl < VERBOSE) return;
        va_list ap; va_start(ap, fmt);
        emit("V", tag, fmt, ap);
        va_end(ap);
    }

    // Sink is called from emit() on every formatted line, in the
    // caller's task. Must be fast — non-blocking only.
    using LogSink = void(*)(char sev, const char* tag, const char* msg, void* ctx);
    void setSink(LogSink fn, void* ctx = nullptr);
    void clearSink();

private:
    Level lvl = ERROR;
    // Mutable so emit() can call the sink without a const_cast.
    mutable LogSink sink_fn_  = nullptr;
    mutable void*   sink_ctx_ = nullptr;

    Log() {}
    Log(const Log&)            = delete;
    Log& operator=(const Log&) = delete;

    void emit(const char* sev, const char* tag,
              const char* fmt, va_list ap) const;
};

} // namespace autolink
