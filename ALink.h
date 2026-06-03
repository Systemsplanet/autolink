#pragma once
#include "ILink.h"
#include "RingBuffer.h"

enum St { OK, SWP, LCK };

class ALink {
    ILink* hw;
    bool isM;
    volatile St st;
    volatile int errs;
    volatile int spdI;
    int spds[5] = {9600, 19200, 38400, 57600, 115200};
    volatile int scores[5];
    RingBuffer rxBuf;

public:
    ALink(ILink* hw, bool isMaster);
    
    void err(); 
    int available();
    int read(uint8_t* b, int max_len);
    void write(const uint8_t* b, int len);
    St getSt() { return st; }
    
    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};
