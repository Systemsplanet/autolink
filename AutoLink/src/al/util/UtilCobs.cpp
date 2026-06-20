// UtilCobs.cpp — COBS encoder/decoder implementation.
// See UtilCobs.h for the public interface.
#include "al/util/UtilCobs.h"
#include <string.h>

namespace autolink {

size_t UtilCobs::encode(const uint8_t* src, size_t len, uint8_t* dst) {
    size_t read_index = 0, write_index = 1, code_index = 0;
    uint8_t code = 1;
    while (read_index < len) {
        if (src[read_index] == 0) {
            dst[code_index] = code; code = 1;
            code_index = write_index++; read_index++;
        } else {
            dst[write_index++] = src[read_index++]; code++;
            if (code == 0xFF) { dst[code_index] = code; code = 1; code_index = write_index++; }
        }
    }
    dst[code_index] = code;
    return write_index;
}

// One memcpy per code group; the implied zero between groups is written inline.
size_t UtilCobs::decode(const uint8_t* src, size_t len, uint8_t* dst) {
    size_t read_index = 0, write_index = 0;
    while (read_index < len) {
        uint8_t code = src[read_index++];
        if (code == 0) return 0;            // malformed: zero code is illegal
        size_t run = code - 1;              // non-zero bytes in this group
        if (read_index + run > len) return 0;
        if (run > 0) {
            memcpy(dst + write_index, src + read_index, run);
            write_index += run;
            read_index  += run;
        }
        if (code < 0xFF && read_index < len) {
            dst[write_index++] = 0;
        }
    }
    return write_index;
}

} // namespace autolink
