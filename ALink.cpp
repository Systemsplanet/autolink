#include "ALink.h"

ALink::ALink(ILink* h, bool m) {
    hw = h;
    isM = m;
    st = OK;
    errs = 0;
    t = 0;
    spdI = 0;
    for(int i=0; i<5; i++) scores[i]=0;
}

void ALink::err() {
    errs++;
    // Threshold: 5 CRC errors triggers resync
    if (errs > 5 && st == OK) {
        st = BRK;
        t = hw->ms();
    }
}

void ALink::tick() {
    uint32_t now = hw->ms();
    
    if (st == OK) {
        // Optional: clear errors periodically if no new ones occur
        if (errs > 0 && now - t > 5000) { errs = 0; t = now; }
        return;
    }

    if (st == BRK) {
        if (isM) hw->brk();
        if (now - t > 100) { // 100ms break
            st = SWP;
            spdI = 0;
            hw->setSpd(spds[0]);
            for(int i=0; i<5; i++) scores[i]=0;
            t = now;
        }
    }
    else if (st == SWP) {
        if (now - t < 50) {
            // Test period (50ms per speed)
            if (isM) {
                uint8_t ping = 0x55;
                hw->tx(&ping, 1);
            } else {
                uint8_t b;
                if (hw->rx(&b, 1) > 0 && b == 0x55) scores[spdI]++;
            }
        } else {
            // Move to next speed
            spdI++;
            if (spdI > 4) {
                st = LCK;
                hw->setSpd(9600); // 9600 is reliable for locking
                t = now;
            } else {
                hw->setSpd(spds[spdI]);
                t = now;
            }
        }
    }
    else if (st == LCK) {
        if (isM) {
            // Master requests best speed
            uint8_t req = 0xAA;
            hw->tx(&req, 1);
            
            uint8_t res;
            if (hw->rx(&res, 1) > 0) {
                hw->setSpd(spds[res]);
                st = OK;
                errs = 0;
                t = now;
            }
        } else {
            // Slave decides best speed
            uint8_t req;
            if (hw->rx(&req, 1) > 0 && req == 0xAA) {
                int bestI = 0;
                for (int i=1; i<5; i++) {
                    if (scores[i] >= scores[bestI]) bestI = i; // Prefer higher spd if tied
                }
                uint8_t res = bestI;
                hw->tx(&res, 1);
                hw->setSpd(spds[bestI]);
                st = OK;
                errs = 0;
                t = now;
            }
        }
        if (now - t > 1000) st = BRK; // Timeout during lock, retry
    }
}

int ALink::rx(uint8_t* b, int n) {
    if (st != OK) return 0; // Block comms while resyncing
    return hw->rx(b, n);
}

void ALink::tx(const uint8_t* b, int n) {
    if (st != OK) return; // Block comms while resyncing
    hw->tx(b, n);
}

St ALink::getSt() { 
    return st; 
}
