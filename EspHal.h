#pragma once
#include "ILink.h"
#include <Arduino.h>

class EspHal : public ILink {
    HardwareSerial* s;
    int rxP, txP;
public:
    EspHal(HardwareSerial* ser, int r, int t) : s(ser), rxP(r), txP(t) {}
    
    void setSpd(int spd) override {
        s->end();
        s->begin(spd, SERIAL_8N1, rxP, txP);
    }
    
    void brk() override {
        s->end();
        pinMode(txP, OUTPUT);
        digitalWrite(txP, LOW);
        delay(20);
        digitalWrite(txP, HIGH);
    }
    
    int rx(uint8_t* b, int len) override {
        int n = 0;
        while(s->available() && n < len) {
            b[n++] = s->read();
        }
        return n;
    }
    
    void tx(const uint8_t* b, int len) override {
        s->write(b, len);
    }
    
    uint32_t ms() override {
        return millis();
    }
};
