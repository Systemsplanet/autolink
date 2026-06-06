#pragma once
#include "ILink.h"
#include <vector>
#include <stdint.h>
#include <stddef.h>

namespace autolink {

enum class State { OK, SWP, LCK };

const char* StateToStr(State s);

// Both command bytes must not collide with preamble bytes 0xAA or 0x55.
constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t REQ_CMD  = 0x11;

struct AutoLinkConfig {
    std::vector<uint32_t> allowedBauds = {9600, 19200, 38400, 57600, 115200};
    int errThreshold = 5;
    int delayMs = 50;
    bool reliableMode = false;
    size_t rxBufferSize = 1024;
    size_t streamBufferSize = 2048;
};

class ALink {
    ILink& hw;
    bool isMaster;
    AutoLinkConfig cfg;
    
    State state;
    int errs;
    int spdI;
    std::vector<int> scores;

    uint8_t rxBuf[4];
    int rxIdx;
    
    uint8_t relRxBuf[256];
    int relRxIdx;

    uint8_t calcCrc(const uint8_t* data, int len) const;
    void sendFrame(uint8_t payload);
    void changeState_unlocked(State newState);
    
    size_t cobsEncode(const uint8_t *ptr, size_t length, uint8_t *dst) const;
    size_t cobsDecode(const uint8_t *ptr, size_t length, uint8_t *dst) const;

public:
    ALink(ILink& hw, bool isMasterNode, const AutoLinkConfig& config = AutoLinkConfig());

    void begin(); // Kicks off baud negotiation; must be called after HAL begin()
    
    void err(); 
    void clearErr();
    
    int available() const;
    int peek();
    int read(); // FIXED: Added missing no-argument read declaration
    int read(uint8_t* b, int max_len);
    void write(const uint8_t* b, int len);
    void flush();
    
    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;
    
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};

} // namespace autolink

