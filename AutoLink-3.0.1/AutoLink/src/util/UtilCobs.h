#pragma once
#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// UtilCobs — Consistent Overhead Byte Stuffing codec.
//
// Removes every 0x00 from a payload so 0x00 can serve as an unambiguous frame
// delimiter on the wire. encode() adds at most one overhead byte per 254
// payload bytes; decode() rejects malformed input by returning 0. Stateless.
// ----------------------------------------------------------------------------

namespace autolink {

class UtilCobs {
public:
    // Worst-case encoded size for n payload bytes (excludes delimiters).
    static size_t encodedMax(size_t n) { return n + n / 254 + 1; }

    // Encode src[0..len) into dst; returns encoded length. dst must hold
    // encodedMax(len) bytes. The output contains no 0x00.
    static size_t encode(const uint8_t* src, size_t len, uint8_t* dst);

    // Decode src[0..len) (one frame, no delimiters) into dst; returns
    // decoded length, or 0 if the input is malformed. dst must hold len bytes.
    static size_t decode(const uint8_t* src, size_t len, uint8_t* dst);
};

} // namespace autolink
