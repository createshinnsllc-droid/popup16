// Minimal PNG writer (8-bit RGB, deflate "stored" blocks): no compression library needed, so the
// Mac tools and the Quest app can both write cover images that any decoder reads.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace png {

inline uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xedb88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        init = true;
    }
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

// rgb: w*h*3 bytes, rows top to bottom
inline bool writeRGB(const std::string &path, int w, int h, const uint8_t *rgb) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01};  // zlib header, then stored blocks of up to 65535 bytes
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back((adler >> (i * 8)) & 255);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        uint32_t n = (uint32_t)data.size();
        for (int i = 3; i >= 0; i--) out.push_back((n >> (i * 8)) & 255);
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t c = ~crc(&out[start], out.size() - start);
        for (int i = 3; i >= 0; i--) out.push_back((c >> (i * 8)) & 255);
    };
    std::vector<uint8_t> ihdr(13, 0);
    for (int i = 0; i < 4; i++) { ihdr[i] = (w >> (24 - 8 * i)) & 255; ihdr[4 + i] = (h >> (24 - 8 * i)) & 255; }
    ihdr[8] = 8; ihdr[9] = 2;  // 8-bit truecolour
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// RGB565 frame (as the SNES core produces) to an RGB PNG
inline bool write565(const std::string &path, int w, int h, const uint16_t *px) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t c = px[i];
        rgb[i * 3] = ((c >> 11) & 31) * 255 / 31;
        rgb[i * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
        rgb[i * 3 + 2] = (c & 31) * 255 / 31;
    }
    return writeRGB(path, w, h, rgb.data());
}

}  // namespace png
