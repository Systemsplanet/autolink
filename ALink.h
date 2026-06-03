#pragma once
#include "ILink.h"

// States: OK (Normal), BRK (Break sent/detected), SWP (Sweeping baud), LCK (Locking baud)
enum St { OK, BRK, SWP, LCK };

class ALink {
    ILink* hw;
    St st;
    int errs;
    uint32_t t;     // timer
    int spdI;       // speed index
    int spds[5] = {9600, 19200, 38400, 57600, 115200};
    int scores[5];
    bool isM;       // is master

public:
    ALink(ILink* h, bool master);
    
    // Call this from your upper protocol when a CRC fails
    void err(); 
    
    // Call this in your main loop
    void tick(); 
    
    // Layered API to send/receive data
    int rx(uint8_t* b, int n);
    void tx(const uint8_t* b, int n);
    
    St getSt();
};
