#include "al/web/OtaCore.h"
#include <string.h>

namespace autolink {

bool otaSafeEntryName(const char *name, char *out, size_t outLen) {
    if (!name || !out)
        return false;
    size_t n = strlen(name);
    if (n == 0 || n > OtaZipStream::kNameMax)
        return false;
    if (name[0] == '/' || name[n - 1] == '/')
        return false;
    if (strstr(name, "..") || strchr(name, '\\'))
        return false;
    constexpr const char *kPrefix = "/web/";
    if (outLen < strlen(kPrefix) + n + 1)
        return false;
    strcpy(out, kPrefix);
    strcat(out, name);
    return true;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

bool OtaZipStream::feed(const uint8_t *b, size_t n) {
    if (st_ == St::ERR)
        return false;
    while (n > 0 || (st_ == St::DATA && dataLeft_ == 0)) {
        switch (st_) {
        case St::SIG: {
            // Accumulate the 4-byte signature into hdr_.
            while (got_ < 4 && n > 0) {
                hdr_[got_++] = *b++;
                n--;
            }
            if (got_ < 4)
                return true;
            uint32_t sig = rd32(hdr_);
            if (sig == 0x02014b50u || sig == 0x06054b50u) {
                // Central directory / end record: archive done.
                st_ = St::DONE;
                return true;
            }
            if (sig != 0x04034b50u)
                return fail("bad local header signature");
            st_ = St::HDR;
            got_ = 0;
            need_ = 26; // rest of the 30-byte local header
            break;
        }
        case St::HDR: {
            while (got_ < need_ && n > 0) {
                hdr_[got_++] = *b++;
                n--;
            }
            if (got_ < need_)
                return true;
            uint16_t flags = rd16(hdr_ + 2);
            uint16_t method = rd16(hdr_ + 4);
            dataLeft_ = rd32(hdr_ + 14); // compSize
            nameLen_ = rd16(hdr_ + 22);
            extraLen_ = rd16(hdr_ + 24);
            if (method != 0)
                return fail("compressed entry (zip with -0)");
            if (flags & 0x0008u)
                return fail("data-descriptor entry unsupported");
            if (nameLen_ == 0 || nameLen_ > kNameMax)
                return fail("entry name too long");
            st_ = St::NAME;
            got_ = 0;
            break;
        }
        case St::NAME: {
            while (got_ < nameLen_ && n > 0) {
                name_[got_++] = (char)*b++;
                n--;
            }
            if (got_ < nameLen_)
                return true;
            name_[nameLen_] = '\0';
            st_ = St::EXTRA;
            got_ = 0;
            break;
        }
        case St::EXTRA: {
            size_t skip = extraLen_ - got_;
            if (skip > n)
                skip = n;
            got_ += skip;
            b += skip;
            n -= skip;
            if (got_ < extraLen_)
                return true;
            uint32_t sz = rd32(hdr_ + 18); // uncompSize == compSize (STORED)
            deliver_ = cb_.entry ? cb_.entry(cb_.ctx, name_, sz) : false;
            st_ = deliver_ ? St::DATA : St::SKIP;
            got_ = 0;
            break;
        }
        case St::DATA:
        case St::SKIP: {
            if (dataLeft_ == 0) {
                if (st_ == St::DATA && cb_.entryEnd && !cb_.entryEnd(cb_.ctx))
                    return fail("entryEnd abort");
                st_ = St::SIG;
                got_ = 0;
                break;
            }
            size_t take = dataLeft_ > n ? n : (size_t)dataLeft_;
            if (take == 0)
                return true;
            if (st_ == St::DATA && cb_.data && !cb_.data(cb_.ctx, b, take))
                return fail("data sink abort");
            b += take;
            n -= take;
            dataLeft_ -= (uint32_t)take;
            break;
        }
        case St::DONE:
            return true; // trailing central-dir bytes ignored
        case St::ERR:
            return false;
        }
    }
    return true;
}

} // namespace autolink
