#pragma once
#include "ILink.h"
#include <vector>
#include <stdint.h>
#include <stddef.h>

namespace autolink {

enum class State { OK, SWP, LCK };

const char* StateToStr(State s);

// Command bytes must not collide with preamble bytes 0xAA or 0x55.
constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t REQ_CMD  = 0x11;

// Max user payload per wire frame (before COBS + CRC8). Drives the static
// scratch buffers below; keep <= 251 so a frame fits in 256 bytes.
constexpr int MAX_CHUNK = 250;

// Message-layer header: len(4, LE) + crc16(2, LE) of the payload.
constexpr int MSG_HDR = 6;

struct AutoLinkConfig {
    std::vector<uint32_t> allowedBauds = {9600, 19200, 38400, 57600, 115200};
    int errThreshold = 5;
    int delayMs = 50;
    bool reliableMode = true;          // framed bytes + message API on by default
    size_t rxBufferSize = 1024;
    size_t streamBufferSize = 2048;
    // Largest message send()/recv() will accept. The AutoLink facade auto-grows
    // streamBufferSize to fit this, so you normally set only this (or nothing).
    size_t maxMsg = 1024;
};

class ALink {
    ILink& hw;
    bool isMaster;
    AutoLinkConfig cfg;

    State state;
    int errs;
    int spdI;
    std::vector<int> scores;

    uint8_t rxBuf[4];
    int rxIdx;

    uint8_t relRxBuf[256];
    int relRxIdx;

    // Message reassembly state (read side).
    int      rxMsgLen;   // -1 = waiting on header
    uint16_t rxMsgCrc;

    // Throughput counters (app stream bytes).
    uint64_t txBytes;
    uint64_t rxBytes;

    uint8_t  calcCrc(const uint8_t* data, int len) const;
    uint16_t calcCrc16(const uint8_t* data, int len) const;
    void sendFrame(uint8_t payload);
    void changeState_unlocked(State newState);
    int  bestSpd_unlocked() const;        // highest baud index that scored > 0
    int  readStream(uint8_t* b, int n);   // pull up to n bytes from the app buffer

    size_t cobsEncode(const uint8_t *ptr, size_t length, uint8_t *dst) const;
    size_t cobsDecode(const uint8_t *ptr, size_t length, uint8_t *dst) const;

public:
    ALink(ILink& hw, bool isMasterNode, const AutoLinkConfig& config = AutoLinkConfig());

    void begin(); // Kicks off baud negotiation; must be called after HAL begin()

    void err();
    void clearErr();

    // Byte-stream API (Stream-compatible).
    int  available() const;
    int  peek();
    int  read();
    int  read(uint8_t* b, int max_len);
    int  write(const uint8_t* b, int len);   // returns bytes accepted while OK
    void flush();

    // Message API (length + CRC16 framed; preserves boundaries). Requires
    // reliableMode for integrity. sendMsg returns true if fully queued.
    // recvMsg returns >0 = message length, 0 = nothing complete yet, -1 = error/drop.
    bool sendMsg(const uint8_t* b, int len);
    int  recvMsg(uint8_t* b, int max_len);

    // Throughput. Counters are app-stream bytes since the last reset.
    void getStats(uint64_t& tx, uint64_t& rx) const;
    void resetStats();

    State getState() const;
    int getErrCount() const;
    int getCurrentSpdIndex() const;

    void onRx(const uint8_t* data, int len);
    void onBreak();
    void onTimer();
};

} // namespace autolink
