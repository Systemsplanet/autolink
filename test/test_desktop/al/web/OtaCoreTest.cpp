// Pins the OTA decision core (todo Open 3): slot
// selection, receive-chunk sizing, entry-name
// sanitizing, and the streaming STORED-zip parser --
// including feeds split at every byte boundary, since
// httpd_req_recv gives no alignment guarantees. The
// esp_ota_* / LittleFS I/O is cross-compile-only; this
// test owns everything decidable on host. Toggle-off
// (e.g. drop the method!=0 reject, or the '..' guard)
// turns the matching case red.
#ifndef ARDUINO

#    include <cassert>
#    include <cstring>
#    include <iostream>
#    include <string>
#    include <vector>
#    include "al/web/OtaCore.h"

using namespace autolink;

namespace {

// Build a STORED zip in memory: local headers + central
// directory end record. crc is irrelevant to the parser.
void putLE16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}
void putLE32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; i++)
        v.push_back((uint8_t)(x >> (8 * i)));
}
void addEntry(std::vector<uint8_t> &v, const std::string &name,
              const std::string &body, uint16_t method = 0, uint16_t flags = 0,
              uint16_t extraLen = 0) {
    putLE32(v, 0x04034b50u);
    putLE16(v, 20);     // version
    putLE16(v, flags);  // flags
    putLE16(v, method); // method
    putLE16(v, 0);      // time
    putLE16(v, 0);      // date
    putLE32(v, 0);      // crc
    putLE32(v, (uint32_t)body.size());
    putLE32(v, (uint32_t)body.size());
    putLE16(v, (uint16_t)name.size());
    putLE16(v, extraLen);
    v.insert(v.end(), name.begin(), name.end());
    for (int i = 0; i < extraLen; i++)
        v.push_back(0xEE);
    v.insert(v.end(), body.begin(), body.end());
}
void endArchive(std::vector<uint8_t> &v) { putLE32(v, 0x02014b50u); }

struct Cap {
    std::vector<std::string> names;
    std::vector<std::string> bodies;
    std::string cur;
    bool skipNamed = false;
};
bool capEntry(void *c, const char *name, uint32_t) {
    Cap *s = (Cap *)c;
    if (s->skipNamed && strcmp(name, "skip.me") == 0)
        return false;
    s->names.push_back(name);
    s->cur.clear();
    return true;
}
bool capData(void *c, const uint8_t *b, size_t n) {
    ((Cap *)c)->cur.append((const char *)b, n);
    return true;
}
bool capEnd(void *c) {
    Cap *s = (Cap *)c;
    s->bodies.push_back(s->cur);
    return true;
}

bool parse(const std::vector<uint8_t> &z, Cap &cap, size_t feedSz) {
    OtaZipCbs cb{ &cap, capEntry, capData, capEnd };
    OtaZipStream p(cb);
    for (size_t off = 0; off < z.size(); off += feedSz) {
        size_t n = z.size() - off < feedSz ? z.size() - off : feedSz;
        if (!p.feed(z.data() + off, n))
            return false;
    }
    return p.done();
}

} // namespace

int main() {
    // --- slot selection -------------------------------
    assert(otaInactiveSlot(0, 2) == 1);
    assert(otaInactiveSlot(1, 2) == 0);
    assert(otaInactiveSlot(0, 1) == -1); // single-slot table
    assert(otaInactiveSlot(-1, 2) == -1);
    assert(otaInactiveSlot(2, 2) == -1);

    // --- recv chunk sizing ----------------------------
    assert(otaRecvChunkBytes(0, OTA_HEAP_RESERVE) == 4096);      // unknown
    assert(otaRecvChunkBytes(10000, OTA_HEAP_RESERVE) == 1024);  // tight
    assert(otaRecvChunkBytes(24576, OTA_HEAP_RESERVE) == 2048);  // mid
    assert(otaRecvChunkBytes(200000, OTA_HEAP_RESERVE) == 8192); // cap

    // --- name sanitizing ------------------------------
    char out[128];
    assert(otaSafeEntryName("index.html", out, sizeof(out)) &&
           std::string(out) == "/web/index.html");
    assert(otaSafeEntryName("css/app.css", out, sizeof(out)) &&
           std::string(out) == "/web/css/app.css");
    assert(!otaSafeEntryName("../etc/pw", out, sizeof(out)));
    assert(!otaSafeEntryName("/abs.html", out, sizeof(out)));
    assert(!otaSafeEntryName("dir/", out, sizeof(out)));
    assert(!otaSafeEntryName("a\\b.html", out, sizeof(out)));
    assert(!otaSafeEntryName("", out, sizeof(out)));

    // --- zip: two entries, whole-buffer feed ----------
    std::vector<uint8_t> z;
    addEntry(z, "index.html", "<html>hi</html>");
    addEntry(z, "app.js", "let x=1;", 0, 0, /*extraLen=*/7);
    endArchive(z);
    {
        Cap cap;
        assert(parse(z, cap, z.size()));
        assert(cap.names.size() == 2 && cap.names[0] == "index.html" &&
               cap.names[1] == "app.js");
        assert(cap.bodies[0] == "<html>hi</html>" &&
               cap.bodies[1] == "let x=1;");
    }

    // --- zip: split feeds at EVERY boundary -----------
    for (size_t fs = 1; fs <= z.size(); fs++) {
        Cap cap;
        if (!parse(z, cap, fs) || cap.bodies.size() != 2 ||
            cap.bodies[0] != "<html>hi</html>" || cap.bodies[1] != "let x=1;") {
            std::cout << "FAIL: split-feed size " << fs << std::endl;
            assert(false);
        }
    }

    // --- zip: entry skip ------------------------------
    {
        std::vector<uint8_t> zs;
        addEntry(zs, "skip.me", "junkjunk");
        addEntry(zs, "keep.txt", "kept");
        endArchive(zs);
        Cap cap;
        cap.skipNamed = true;
        assert(parse(zs, cap, 3));
        assert(cap.names.size() == 1 && cap.names[0] == "keep.txt" &&
               cap.bodies[0] == "kept");
    }

    // --- zip: rejects ---------------------------------
    {
        std::vector<uint8_t> zd;
        addEntry(zd, "a.txt", "x", /*method=*/8); // deflate
        endArchive(zd);
        Cap cap;
        assert(!parse(zd, cap, zd.size()));
    }
    {
        std::vector<uint8_t> zf;
        addEntry(zf, "a.txt", "x", 0, /*flags=*/0x0008); // data descriptor
        endArchive(zf);
        Cap cap;
        assert(!parse(zf, cap, zf.size()));
    }
    {
        std::vector<uint8_t> zb = { 'n', 'o', 'p', 'e', 0, 0 };
        Cap cap;
        assert(!parse(zb, cap, zb.size())); // bad signature
    }

    std::cout << "PASS: OtaCore (slots, chunk sizing, names, zip stream)"
              << std::endl;
    return 0;
}

#endif
