// COBS encode/decode.
// Zero byte is the wire frame delimiter.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace autolink
{
class UtilCobs
{
public:
    static size_t encodedMax(size_t n) { return n + n / 254 + 1; }

    static size_t encode(const uint8_t *src, size_t len, uint8_t *dst);

    static size_t decode(const uint8_t *src, size_t len, uint8_t *dst);
};

} // namespace autolink