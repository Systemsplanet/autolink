#pragma once
#include "ILink.h"
#include <vector>
#include <stdint.h>

enum class State { OK, SWP, LCK };

// Helper for human-readable state logs
const char* StateToStr(State s);

const uint8_t ALINK_PING_CMD = 0x55;
const uint8_t ALINK_REQ_CMD = 0xAA;

class ALink {
    ILink& hw;
    bool isMaster;
    
    // Protected by hardware lock()
    State state;
    int errs;
    int spdI;
    std::vector<int> scores;
    
    // Configuration
    std::vector<uint32_t> spds;
    int errThreshold;
    int timerDelayMs;

    // Frame Parsing Buffer
    uint8_t rxBuf[4];
    int rxIdx;

    uint8_t calcCrc(const uint8_t* data, int len) const;
    void sendFrame(uint8_t payload);
    void changeState_unlocked(State newState);

public:
    ALink(ILink& hw, bool isMasterNode, 
          const std::vector<uint32_t>& allowedBauds = {9600, 19200, 38400, 57600, 115200}, 
          int errorThreshold = 5, 
          int delayMs = 50);
    
    void err(); 
    void clearErr(); // Resolves the leaky error accumulation bug
    
    int available() const;
    int read(uint8_t* b, int max_len);
    void write(const uint8_t* b, int len);
    
    // Testability / Introspection getters
    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;
    
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};
