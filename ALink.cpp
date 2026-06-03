#include "ALink.h"

ALink::ALink(ILink& h, bool isMasterNode, const std::vector<int>& allowedBauds, int errorThreshold, int delayMs)
    : hw(h), isMaster(isMasterNode), spds(allowedBauds), errThreshold(errorThreshold), timerDelayMs(delayMs),
      state(State::OK), errs(0), spdI(0) 
{
    scores.resize(spds.size(), 0);
    hw.bind(this);
}

void ALink::err() {
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        return;
    }
    errs++;
    bool trigger = (errs > errThreshold);
    hw.unlock();

    if (trigger) {
        hw.sendBreak();
        onBreak(); 
    }
}

int ALink::available() const { 
    return hw.appBufAvailable(); 
}

int ALink::read(uint8_t* b, int max_len) {
    int n = 0;
    while(n < max_len && hw.appBufAvailable() > 0) {
        int val = hw.popAppBuf();
        if(val >= 0) b[n++] = (uint8_t)val;
    }
    return n;
}

void ALink::write(const uint8_t* b, int len) {
    hw.lock();
    State s = state;
    hw.unlock();
    
    if (s == State::OK) { 
        hw.tx(b, len); 
    }
}

State ALink::getState() const { 
    hw.lock();
    State s = state;
    hw.unlock();
    return s;
}

int ALink::getErrCount() const {
    hw.lock();
    int e = errs;
    hw.unlock();
    return e;
}

int ALink::getCurrentSpdIndex() const {
    hw.lock();
    int idx = spdI;
    hw.unlock();
    return idx;
}

void ALink::onRx(const uint8_t* data, int len) {
    hw.lock();
    State current_state = state;
    hw.unlock();

    for(int i=0; i<len; i++) {
        uint8_t b = data[i];
        
        if (current_state == State::OK) {
            hw.pushAppBuf(b);
        } 
        else if (current_state == State::SWP) {
            hw.lock();
            if (b == ALINK_PING_CMD && spdI < (int)spds.size()) scores[spdI]++;
            hw.unlock();
        } 
        else if (current_state == State::LCK) {
            if (isMaster) {
                if (b < (int)spds.size()) { 
                    hw.setSpd(spds[b]);
                    hw.lock();
                    state = State::OK; errs = 0;
                    current_state = State::OK; // Update local copy
                    hw.unlock();
                }
            } else {
                if (b == ALINK_REQ_CMD) { 
                    int best = 0;
                    hw.lock();
                    for(int j=1; j<(int)spds.size(); j++) {
                        if (scores[j] >= scores[best]) best = j;
                    }
                    hw.unlock();
                    
                    uint8_t res = best;
                    hw.tx(&res, 1);
                    hw.flushTx();
                    hw.setSpd(spds[best]);
                    
                    hw.lock();
                    state = State::OK; errs = 0;
                    current_state = State::OK; // Update local copy
                    hw.unlock();
                }
            }
        }
    }
}

void ALink::onBreak() {
    hw.lock();
    state = State::SWP;
    spdI = 0;
    for(int i=0; i<(int)scores.size(); i++) scores[i]=0;
    hw.unlock();
    
    hw.clearAppBuf();
    hw.setSpd(spds[0]);
    hw.startTimer(timerDelayMs); 
}

void ALink::onTimer() {
    hw.lock();
    State s = state;
    int curSpd = spdI;
    hw.unlock();
    
    if (s == State::SWP) {
        if (isMaster && curSpd < (int)spds.size()) {
            uint8_t ping = ALINK_PING_CMD;
            hw.tx(&ping, 1);
            hw.flushTx();
        }
        
        hw.lock();
        spdI++;
        curSpd = spdI;
        hw.unlock();
        
        if (curSpd < (int)spds.size()) {
            hw.setSpd(spds[curSpd]);
            hw.startTimer(timerDelayMs);
        } else {
            hw.lock();
            state = State::LCK;
            hw.unlock();
            
            hw.setSpd(spds[0]);
            if (isMaster) hw.startTimer(timerDelayMs); 
        }
    } 
    else if (s == State::LCK && isMaster) {
        uint8_t req = ALINK_REQ_CMD;
        hw.tx(&req, 1);
        hw.flushTx();
    }
}
