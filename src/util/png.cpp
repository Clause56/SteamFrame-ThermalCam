#include "png.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    c ^= 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((x >> (8 * i)) & 0xff);
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32(td.data(), td.size()));
}

}  // namespace

bool writePng(const std::string& path, const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    // zlib stream of stored (uncompressed) deflate blocks.
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    size_t off = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);  // BFINAL on the last block
        z.push_back(n & 0xff);
        z.push_back(n >> 8);
        z.push_back(~n & 0xff);
        z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    } while (off < raw.size());
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    return fclose(f) == 0 && ok;
}
