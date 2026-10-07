// Diorama model: every SNES layer becomes a stack of sheets at real depths.
// Input: the composited frame plus each main-screen layer drawn on its own (patched snes9x core).
// Output: six texture slices (BG1-4, sprites, backdrop; alpha = SNES priority depth) and one quad
// per occupied row span of each (layer, priority) sheet, tagged with a disparity in SNES pixels.
// Sheets are ordered exactly like SNES priorities, so seen from straight ahead they reproduce the
// original frame; moving or using two eyes reveals the depth.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace diorama {

static const int TEX_W = 512, TEX_H = 480, SLICES = 6;  // slices 0-3 BG1-4, 4 sprites, 5 backdrop
static const float M7_FAR = 7.0f;

struct Quad {
    float u0, u1, v;     // texel columns [u0, u1), row v
    float disparity;     // SNES pixels, + = behind the screen
    float slice, z;      // texture slice and the priority depth this sheet shows
};

struct Input {
    unsigned w, h;
    const uint16_t *rgb565;        // composited frame, w x h, tightly packed
    const uint8_t *layer;          // owner layer id per pixel (0-3 BG, 4 OBJ, 5 backdrop, 6/7 Mode 7)
    const uint8_t *zmain;          // priority depth of the owner pixel
    const uint16_t *planeColor[5]; // each layer alone, w x h tightly packed
    const uint8_t *planeZ[5];      // 0 = empty
};

// Priority depth (z - 32) to disparity. Sheets must get strictly nearer as SNES priority rises,
// otherwise a sprite could float in front of a tile that covers it. Modes 0/1 and 2-7 interleave
// their layers differently, so each gets its own table.
inline float disparityFor(int p, bool mode01) {
    static const float m01[18] = {7, 7, 6.5f, 6, 5.5f, 5.25f, 5, 4.5f, 3.5f, 3.25f, 3, 1, 0.5f, 0.375f, 0.25f, 0, -0.5f, -1.5f};
    static const float m27[18] = {7, 7, 6, 3.5f, 2.5f, 2.25f, 2, 1, 0.75f, 0.7f, 0.65f, 0.6f, 0.5f, 0.375f, 0.25f, 0, -0.5f, -1.5f};
    if (p < 0) p = 0;
    if (p > 17) p = 17;
    return mode01 ? m01[p] : m27[p];
}

// "Pop-up look": what a lit paper diorama would show. Baked into the sheet textures on the CPU,
// so the headset GPU only samples them.
struct Look {
    bool on = true;
    float shadow = 0.45f;     // darkening where a nearer sheet blocks the top-left light
    float rim = 0.16f;        // cut-paper edge: lit top-left edge, shaded bottom-right edge
    float haze = 0.022f;      // per SNES pixel of depth, toward a cool distance tint (max 0.22)
};

struct Builder {
    std::vector<uint32_t> tex;   // SLICES x TEX_H x TEX_W, RGBA bytes (R in the low byte)
    std::vector<Quad> quads;
    bool mode7Ramp = true;
    bool showBackdrop = true;   // false: no backdrop sheet, so the room shows through behind the game
    Look look;
    uint32_t lut[65536];

    Builder() : tex((size_t)SLICES * TEX_H * TEX_W, 0) {
        for (int i = 0; i < 65536; i++) {
            uint32_t r = ((i >> 11) & 31) * 255 / 31, g = ((i >> 5) & 63) * 255 / 63, b = (i & 31) * 255 / 31;
            lut[i] = (b << 16) | (g << 8) | r;  // alpha added per pixel
        }
    }
    uint32_t *slice(int s) { return &tex[(size_t)s * TEX_H * TEX_W]; }

    void build(const Input &in) {
        const unsigned w = std::min<unsigned>(in.w, TEX_W), h = std::min<unsigned>(in.h, TEX_H);
        quads.clear();

        // which mode family is on screen (BG3 only exists in modes 0/1; BG1 low priority is 11 there, 7 in modes 2+)
        bool mode01 = true;
        {
            bool p7 = false, p11 = false;
            for (size_t i = 0; i < (size_t)w * h; i += 7)
                if (in.planeZ[0][i]) { int p = in.planeZ[0][i] - 32; p7 |= p == 7; p11 |= p == 11; }
            for (size_t i = 0; i < (size_t)w * h && !p11; i += 7) p11 |= in.planeZ[2][i] != 0;
            mode01 = !(p7 && !p11);
        }
        int m7top = -1;
        std::vector<uint8_t> m7row(h, 0);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
                if (in.layer[y * w + x] == 6) { m7row[y] = 1; if (m7top < 0) m7top = (int)y; break; }
        if (!mode7Ramp) m7top = -1;

        // layer slices: the composited colour where this layer owns the pixel (keeps transparency and
        // colour-math effects), its own colour where it is hidden, empty elsewhere
        for (int n = 0; n < 5; n++) {
            uint32_t *dst = slice(n);
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++) {
                    size_t i = (size_t)y * w + x;
                    uint8_t z = in.planeZ[n][i];
                    if (!z) { dst[y * TEX_W + x] = 0; continue; }
                    int owner = in.layer[i];
                    bool owns = (owner == n || (n == 0 && owner == 6) || (n == 1 && owner == 7)) && in.zmain[i] == z;
                    dst[y * TEX_W + x] = lut[owns ? in.rgb565[i] : in.planeColor[n][i]] | ((uint32_t)z << 24);
                }
        }
        // backdrop slice: composited backdrop pixels, gaps filled from the nearest backdrop pixel in the row
        {
            uint32_t *dst = slice(5);
            uint32_t carry = 0;
            for (unsigned y = 0; y < h; y++) {
                int first = -1;
                for (unsigned x = 0; x < w; x++) if (in.layer[y * w + x] == 5) { first = (int)x; break; }
                uint32_t c = first >= 0 ? lut[in.rgb565[y * w + first]] : carry;
                for (unsigned x = 0; x < w; x++) {
                    if (in.layer[y * w + x] == 5) c = lut[in.rgb565[y * w + x]];
                    dst[y * TEX_W + x] = c | (1u << 24);
                }
                carry = c;
            }
        }

        auto rowDisparity = [&](int n, int z, unsigned y) {
            float d;
            if (n == 0 && m7row[y] && m7top >= 0) {
                float t = (float)((int)y - m7top) / std::max(1.0f, (float)((int)h - 1 - m7top));
                return M7_FAR * (1.0f - t) + 1.0f * t;  // floor: horizon far, bottom just behind the playfield
            }
            d = (n == 5) ? 7.5f : (n == 0 && m7row[y]) ? 1.0f : disparityFor(z - 32, mode01);
            if (m7top >= 0 && (int)y < m7top && n != 4 && d > 0.0f) d = M7_FAR + 0.5f * d;  // skyline beyond the horizon
            return d;
        };

        // one quad per row span of each (layer, priority) sheet
        for (int n = 0; n < 5; n++) {
            bool seen[256] = {false};
            for (size_t i = 0; i < (size_t)w * h; i++) seen[in.planeZ[n][i]] = true;
            for (int z = 1; z < 256; z++) {
                if (!seen[z]) continue;
                for (unsigned y = 0; y < h; y++) {
                    const uint8_t *row = &in.planeZ[n][y * w];
                    int x0 = -1, x1 = -1;
                    for (unsigned x = 0; x < w; x++) if (row[x] == z) { if (x0 < 0) x0 = (int)x; x1 = (int)x + 1; }
                    if (x0 >= 0) quads.push_back({(float)x0, (float)x1, (float)y, rowDisparity(n, z, y), (float)n, (float)z});
                }
            }
        }
        if (showBackdrop)
            for (unsigned y = 0; y < h; y++) quads.push_back({0.0f, (float)w, (float)y, rowDisparity(5, 1, y), 5.0f, 1.0f});

        if (look.on) bakeLook(w, h);

        // far to near; equal depth keeps SNES priority order (higher z drawn later)
        std::stable_sort(quads.begin(), quads.end(), [](const Quad &a, const Quad &b) {
            if (a.disparity != b.disparity) return a.disparity > b.disparity;
            return a.z < b.z;
        });
    }

    // per-pixel disparity of every sheet pixel, and the nearest disparity at each screen position
    std::vector<float> own, nearest;

    void bakeLook(unsigned w, unsigned h) {
        const size_t plane = (size_t)TEX_H * TEX_W;
        own.assign((size_t)SLICES * plane, 1e9f);
        nearest.assign((size_t)w * h, 1e9f);
        for (const auto &q : quads) {
            int s = (int)q.slice, y = (int)q.v;
            uint32_t *row = slice(s) + (size_t)y * TEX_W;
            for (int x = (int)q.u0; x < (int)q.u1; x++)
                if ((row[x] >> 24) == (uint32_t)q.z) {
                    own[(size_t)s * plane + (size_t)y * TEX_W + x] = q.disparity;
                    float &n = nearest[(size_t)y * w + x];
                    if (q.disparity < n) n = q.disparity;
                }
        }
        auto nearAt = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return 1e9f;
            return nearest[(size_t)y * w + x];
        };
        for (int s = 0; s < SLICES; s++) {
            uint32_t *t = slice(s);
            const float *od = &own[(size_t)s * plane];
            for (int y = 0; y < (int)h; y++)
                for (int x = 0; x < (int)w; x++) {
                    size_t i = (size_t)y * TEX_W + x;
                    float d = od[i];
                    if (d > 1e8f) continue;
                    uint32_t c = t[i], z = c >> 24;
                    float r = (float)(c & 255), g = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255);
                    float k = 1.0f;
                    // drop shadow: a nearer sheet up-left of here blocks the light (two taps = soft edge)
                    float gap1 = d - nearAt(x - 2, y - 2), gap2 = d - nearAt(x - 4, y - 4);
                    float sh = 0.5f * (std::clamp(gap1 / 1.5f, 0.0f, 1.0f) + std::clamp(gap2 / 1.5f, 0.0f, 1.0f));
                    k -= look.shadow * sh;
                    // cut-paper edge on everything except the backdrop
                    if (s != 5) {
                        auto same = [&](int xx, int yy) {
                            if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h) return true;
                            return (t[(size_t)yy * TEX_W + xx] >> 24) == z;
                        };
                        if (!same(x - 1, y) || !same(x, y - 1)) k += look.rim;
                        else if (!same(x + 1, y) || !same(x, y + 1)) k -= look.rim;
                    }
                    r *= k; g *= k; b *= k;
                    // distance haze toward a cool tint
                    float hz = std::clamp(d * look.haze, 0.0f, 0.22f);
                    r += (150.0f - r) * hz; g += (170.0f - g) * hz; b += (200.0f - b) * hz;
                    auto cl = [](float v) { return (uint32_t)std::clamp(v, 0.0f, 255.0f); };
                    t[i] = cl(r) | (cl(g) << 8) | (cl(b) << 16) | (z << 24);
                }
        }
    }
};

}  // namespace diorama
