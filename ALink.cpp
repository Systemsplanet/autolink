#include "ALink.h"

ALink::ALink(ILink* h, bool isMasterNode, std::vector<int> allowedBauds, int errorThreshold, int delayMs) {
    hw = h;
    isMaster = isMasterNode;
    spds = allowedBauds;
    errThreshold = errorThreshold;
    timerDelayMs = delayMs;
    
    state = OK;
    errs = 0;
    spdI = 0;
    scores.resize(spds.size(), 0);
    
    hw->bind(this);
}

void ALink::err() {
    hw->lock();
    if (state != OK) {
        hw->unlock();
        return;
    }
    errs++;
    bool trigger = (errs > errThreshold);
    hw->unlock();

    if (trigger) {
        hw->sendBreak();
        onBreak(); 
    }
}

int ALink::available() { 
    return hw->appBufAvailable(); 
}

int ALink::read(uint8_t* b, int max_len) {
    int n = 0;
    while(n < max_len && hw->appBufAvailable() > 0) {
        int val = hw->popAppBuf();
        if(val >= 0) b[n++] = (uint8_t)val;
    }
    return n;
}

void ALink::write(const uint8_t* b, int len) {
    hw->lock();
    State s = state;
    hw->unlock();
    
    if (s == OK) { 
        hw->tx(b, len); 
    }
}

State ALink::getState() { 
    hw->lock();
    State s = state;
    hw->unlock();
    return s;
}

int ALink::getErrCount() {
    hw->lock();
    int e = errs;
    hw->unlock();
    return e;
}

int ALink::getCurrentSpdIndex() {
    hw->lock();
    int idx = spdI;
    hw->unlock();
    return idx;
}

void ALink::onRx(const uint8_t* data, int len) {
    hw->lock();
    State current_state = state;
    hw->unlock();

    for(int i=0; i<len; i++) {
        uint8_t b = data[i];
        
        if (current_state == OK) {
            hw->pushAppBuf(b);
        } 
        else if (current_state == SWP) {
            hw->lock();
            if (b == ALINK_PING_CMD && spdI < (int)spds.size()) scores[spdI]++;
            hw->unlock();
        } 
        else if (current_state == LCK) {
            if (isMaster) {
                if (b < spds.size()) { 
                    hw->setSpd(spds[b]);
                    hw->lock();
                    state = OK; errs = 0;
                    current_state = OK; // Update local copy
                    hw->unlock();
                }
            } else {
                if (b == ALINK_REQ_CMD) { 
                    int best = 0;
                    hw->lock();
                    for(size_t j=1; j<spds.size(); j++) {
                        if (scores[j] >= scores[best]) best = j;
                    }
                    hw->unlock();
                    
                    uint8_t res = best;
                    hw->tx(&res, 1);
                    hw->flushTx();
                    hw->setSpd(spds[best]);
                    
                    hw->lock();
                    state = OK; errs = 0;
                    current_state = OK; // Update local copy
                    hw->unlock();
                }
            }
        }
    }
}

void ALink::onBreak() {
    hw->lock();
    state = SWP;
    spdI = 0;
    for(size_t i=0; i<scores.size(); i++) scores[i]=0;
    hw->unlock();
    
    hw->clearAppBuf();
    hw->setSpd(spds[0]);
    hw->startTimer(timerDelayMs); 
}

void ALink::onTimer() {
    hw->lock();
    State s = state;
    int curSpd = spdI;
    hw->unlock();
    
    if (s == SWP) {
        if (isMaster && curSpd < (int)spds.size()) {
            uint8_t ping = ALINK_PING_CMD;
            hw->tx(&ping, 1);
            hw->flushTx();
        }
        
        hw->lock();
        spdI++;
        curSpd = spdI;
        hw->unlock();
        
        if (curSpd < (int)spds.size()) {
            hw->setSpd(spds[curSpd]);
            hw->startTimer(timerDelayMs);
        } else {
            hw->lock();
            state = LCK;
            hw->unlock();
            
            hw->setSpd(spds[0]);
            if (isMaster) hw->startTimer(timerDelayMs); 
        }
    } 
    else if (s == LCK && isMaster) {
        uint8_t req = ALINK_REQ_CMD;
        hw->tx(&req, 1);
        hw->flushTx();
    }
}
