#include "ALink.h"

ALink::ALink(ILink* h, bool master) {
    hw = h;
    isM = master;
    st = OK;
    errs = 0;
    spdI = 0;
    hw->bind(this);
    for(int i=0; i<5; i++) scores[i]=0;
}

void ALink::err() {
    if (st != OK) return;
    errs++;
    if (errs > 5) {
        hw->sendBreak();
        onBreak(); 
    }
}

int ALink::available() { 
    return rxBuf.available(); 
}

int ALink::read(uint8_t* b, int max_len) {
    int n = 0;
    while(n < max_len && rxBuf.available()) {
        b[n++] = (uint8_t)rxBuf.pop();
    }
    return n;
}

void ALink::write(const uint8_t* b, int len) {
    if (st == OK) { 
        hw->tx(b, len); 
    }
}

void ALink::onRx(const uint8_t* data, int len) {
    for(int i=0; i<len; i++) {
        uint8_t b = data[i];
        
        if (st == OK) {
            rxBuf.push(b);
        } 
        else if (st == SWP) {
            if (b == 0x55 && spdI < 5) scores[spdI]++;
        } 
        else if (st == LCK) {
            if (isM) {
                if (b < 5) { 
                    hw->setSpd(spds[b]);
                    st = OK; errs = 0;
                }
            } else {
                if (b == 0xAA) { 
                    int best = 0;
                    for(int j=1; j<5; j++) {
                        if (scores[j] >= scores[best]) best = j;
                    }
                    uint8_t res = best;
                    hw->tx(&res, 1);
                    hw->flushTx();
                    hw->setSpd(spds[best]);
                    st = OK; errs = 0;
                }
            }
        }
    }
}

void ALink::onBreak() {
    st = SWP;
    spdI = 0;
    rxBuf.clear();
    for(int i=0; i<5; i++) scores[i]=0;
    hw->setSpd(spds[0]);
    hw->startTimer(50); 
}

void ALink::onTimer() {
    if (st == SWP) {
        if (isM && spdI < 5) {
            uint8_t ping = 0x55;
            hw->tx(&ping, 1);
            hw->flushTx();
        }
        
        spdI++;
        if (spdI < 5) {
            hw->setSpd(spds[spdI]);
            hw->startTimer(50);
        } else {
            st = LCK;
            hw->setSpd(9600);
            if (isM) hw->startTimer(50); 
        }
    } 
    else if (st == LCK && isM) {
        uint8_t req = 0xAA;
        hw->tx(&req, 1);
        hw->flushTx();
    }
}
