// Arduino stub for host compile-check.
#pragma once
#ifndef Arduino_h
#    define Arduino_h
#    include <stddef.h>
#    include <stdint.h>
#    include <string.h>

typedef bool boolean;
#    define HIGH 1
#    define LOW 0
#    define INPUT 0
#    define OUTPUT 1
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int)
{
    return 0;
}
inline unsigned long millis()
{
    return 0;
}
inline unsigned long micros()
{
    return 0;
}
inline void delay(int) {}
inline long random(long)
{
    return 0;
}
inline long random(long, long)
{
    return 0;
}
inline void randomSeed(unsigned long) {}
class Serial_t
{
public:
    void begin(unsigned long) {}
    int available() { return 0; }
    int read() { return -1; }
    int peek() { return -1; }
    size_t write(const char *, size_t) { return 0; }
    void println(const char *) {}
    void print(const char *) {}
};
extern Serial_t Serial;

class Stream
{
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *, size_t) = 0;
    virtual void flush() {}
    int readBytes(char *buf, int len)
    {
        int n = 0;
        while (n < len && available())
            buf[n++] = (char)read();
        return n;
    }
};

typedef int esp_err_t;
#    define ESP_OK 0
#    define ESP_FAIL -1
#    define ESP_ERR_INVALID_ARG 0x102
#    define ESP_ERR_NO_MEM 0x101
inline const char *esp_err_to_name(esp_err_t)
{
    return "?";
}

typedef int esp_log_level_t;
#    define ESP_LOG_VERBOSE 5
#    define ESP_LOG_DEBUG 4
#    define ESP_LOG_INFO 3
#    define ESP_LOG_WARN 2
#    define ESP_LOG_ERROR 1
inline void esp_log_level_set(const char *, esp_log_level_t) {}

#    include <sys/time.h>
#    include <time.h>
inline int gettimeofday(struct timeval *, void *)
{
    return 0;
}
struct tm *localtime_r(const time_t *, struct tm *);

#endif