#pragma once
#include "ILink.h"
#include <vector>
#include <stdint.h>

namespace autolink {

enum class State { OK, SWP, LCK };

const char* StateToStr(State s);

constexpr uint8_t PING_CMD = 0x55;
constexpr uint8_t REQ_CMD = 0xAA;
constexpr uint8_t RELIABLE_SYNC = 0xDD;

struct AutoLinkConfig {
    std::vector<uint32_t> allowedBauds = {9600, 19200, 38400, 57600, 115200};
    int errThreshold = 5;
    int delayMs = 50;
    bool reliableMode = false;
};

class ALink {
    ILink& hw;
    bool isMaster;
    AutoLinkConfig cfg;
    
    // Protected by hardware lock()
    State state;
    int errs;
    int spdI;
    std::vector<int> scores;

    // Frame Parsing Buffers
    uint8_t rxBuf[4];
    int rxIdx;
    
    uint8_t relRxBuf[3];
    int relRxIdx;

    uint8_t calcCrc(const uint8_t* data, int len) const;
    void sendFrame(uint8_t payload);
    void changeState_unlocked(State newState);

public:
    ALink(ILink& hw, bool isMasterNode, const AutoLinkConfig& config = AutoLinkConfig());
    
    void err(); 
    void clearErr();
    
    int available() const;
    int peek();
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
