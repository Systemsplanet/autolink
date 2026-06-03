#pragma once
#include "ILink.h"
#include <vector>

enum State { OK, SWP, LCK };

class ALink {
    ILink* hw;
    bool isMaster;
    
    // Protected by hardware lock()
    State state;
    int errs;
    int spdI;
    std::vector<int> scores;
    
    // Configuration
    std::vector<int> spds;
    int errThreshold;
    int timerDelayMs;

public:
    ALink(ILink* hw, bool isMasterNode, 
          std::vector<int> allowedBauds = {9600, 19200, 38400, 57600, 115200}, 
          int errorThreshold = 5, 
          int delayMs = 50);
    
    void err(); 
    int available();
    int read(uint8_t* b, int max_len);
    void write(const uint8_t* b, int len);
    
    // Testability / Introspection getters
    State getState();
    int getErrCount();
    int getCurrentSpdIndex();
    
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};
