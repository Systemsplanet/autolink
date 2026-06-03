#pragma once
#include <stdint.h>

class RingBuffer {
    uint8_t buf[256];
    volatile int head = 0;
    volatile int tail = 0;
public:
    void push(uint8_t b) {
        int next = (head + 1) % 256;
        if (next != tail) { buf[head] = b; head = next; }
    }
    
    int pop() {
        if (head == tail) return -1;
        uint8_t b = buf[tail];
        tail = (tail + 1) % 256;
        return b;
    }
    
    int available() {
        if (head >= tail) return head - tail;
        return 256 - tail + head;
    }
    
    void clear() {
        head = tail = 0;
    }
};
