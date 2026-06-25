// Consistent Overhead Byte Stuffing — run-length
// encoded zero delimiter. Used to frame both control
// and data frames; the zero byte never appears inside
// the encoded body.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace autolink
{
class UtilCobs
{
public:
    static size_t encodedMax(size_t n)
    {
        return n + n / 254 + 1;
    }


    static size_t encode(const uint8_t *src,
                         size_t len, uint8_t *dst);


    static size_t decode(const uint8_t *src,
                         size_t len, uint8_t *dst);
};

} // namespace autolink
