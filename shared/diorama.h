// Diorama model: every SNES layer becomes a stack of sheets at real depths.
// Input: the composited frame plus each main-screen layer drawn on its own (patched snes9x core).
// Output: six texture slices (BG1-4, sprites, backdrop; alpha = SNES priority depth) and one quad
// per occupied row span of each (layer, priority) sheet, tagged with a disparity in SNES pixels.
// Sheets are ordered exactly like SNES priorities, so seen from straight ahead they reproduce the
// original frame; moving or using two eyes reveals the depth.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <vector>

namespace diorama {

inline double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static const int TEX_W = 512, TEX_H = 480, SLICES = 6;  // slices 0-3 BG1-4, 4 sprites, 5 backdrop
static const float M7_FAR = 7.0f;

struct Quad {
    float u0, u1, v;     // texel columns [u0, u1), row v
    float disparity;     // SNES pixels, + = behind the screen (top edge)
    float slice, z;      // texture slice and the priority depth this sheet shows
    float disparity1;    // bottom edge (differs on a sloping Mode 7 floor)
    bool m7 = false;     // sample the HD Mode 7 map instead of the sheet colour
    float m7uv[4][2];    // map texel coords at top-left, top-right, bottom-left, bottom-right
};

// Card edge of a sprite span (spriteWalls only): a vertical strip at column u over rows [v0, v1). Its top
// edge sits at disparity dFront and its bottom edge at dBottom, the same two depths the sprite quad uses (they
// differ on a sloping Mode 7 floor). Its depth in metres is added by the renderer. Drawn as geometry, not in the quads.
struct Wall {
    float u, v0, v1;
    float dFront, dBottom;
};

struct Input {
    unsigned w, h;
    const uint16_t *rgb565;        // composited frame, w x h, tightly packed
    const uint8_t *layer;          // owner layer id per pixel (0-3 BG, 4 OBJ, 5 backdrop, 6/7 Mode 7)
    const uint8_t *zmain;          // priority depth of the owner pixel
    const uint16_t *planeColor[5]; // each layer alone, w x h tightly packed
    const uint8_t *planeZ[5];      // 0 = empty
    // optional Mode 7 state (snes3d_get_mode7): per-scanline matrices, VRAM, CGRAM, flags
    const int16_t *m7lines = nullptr;
    const uint8_t *vram = nullptr;
    const uint16_t *cgram = nullptr;
    int m7flags = 0;
    double profMs[5] = {0, 0, 0, 0, 0};  // slices, spans, map, look, sort (last build)
};

// The SNES Mode 7 transform with the matrix of scanline `line`, at continuous screen position
// (xc, yc) in scanline units (pixel x of scanline L covers [x, x+1) x [L, L+1)). Returns 1024x1024
// map texel coordinates, before wrapping.
inline void mode7uv(const int16_t *L, int flags, int line, float xc, float yc, float &u, float &v) {
    auto sx13 = [](int a) { return ((int32_t)a << 19) >> 19; };
    auto clip10 = [](int a) { return (a & 0x2000) ? (a | ~0x3ff) : (a & 0x3ff); };
    const int16_t *m = L + 8 * line;
    int A = m[0], B = m[1], C = m[2], D = m[3];
    int CX = sx13(m[4]), CY = sx13(m[5]), HO = sx13(m[6]), VO = sx13(m[7]);
    float xs = (flags & 1) ? 255.5f - xc : xc - 0.5f;
    float yp = yc + 0.5f;  // scanline L samples y' = L + 1 at its centre (yc = L + 0.5)
    if (flags & 2) yp = 255.0f - yp;
    int xx = clip10(HO - CX), yy = clip10(VO - CY);
    u = (A * xs + (float)A * xx + B * yp + (float)B * yy) / 256.0f + CX;
    v = (C * xs + (float)C * xx + D * yp + (float)D * yy) / 256.0f + CY;
}

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
    std::vector<Wall> walls;    // sprite card edges, filled only when spriteWalls is set
    bool spriteWalls = false;   // true: each sprite span gets card-edge walls (quads and texture unchanged)
    bool mode7Ramp = true;
    bool showBackdrop = true;   // false: no backdrop sheet, so the room shows through behind the game
    Look look;
    // HD Mode 7: the whole 1024x1024 map as RGBA (alpha 0 = colour 0, transparent); rebuilt on change
    std::vector<uint32_t> map7;
    bool map7Dirty = false, frameHasM7 = false;
    uint64_t map7Hash = 0;
    int m7flags = 0;
    double profMs[5] = {0, 0, 0, 0, 0};  // slices, spans, map, look, sort (last build)
    uint32_t lut[65536];

    Builder() : tex((size_t)SLICES * TEX_H * TEX_W, 0) {
        for (int i = 0; i < 65536; i++) {
            uint32_t r = ((i >> 11) & 31) * 255 / 31, g = ((i >> 5) & 63) * 255 / 63, b = (i & 31) * 255 / 31;
            lut[i] = (b << 16) | (g << 8) | r;  // alpha added per pixel
        }
    }
    uint32_t *slice(int s) { return &tex[(size_t)s * TEX_H * TEX_W]; }

    unsigned lastW = 256, lastH = 224;  // frame size of the last build

    void build(const Input &in) {
        const unsigned w = std::min<unsigned>(in.w, TEX_W), h = std::min<unsigned>(in.h, TEX_H);
        lastW = w; lastH = h;
        quads.clear();
        walls.clear();

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

        double t0 = nowMs();
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

        // Mode 7 floor depth from the game's own zoom: a scanline drawn at k times the texel step of the
        // nearest floor line is k times as far away. Rotation-only Mode 7 (no zoom change) stays flat.
        std::vector<float> floorD(h, 1.0f);
        frameHasM7 = false;
        bool slope = false;
        if (in.m7lines && m7top >= 0) {
            int first = in.m7flags >> 9;
            float smin = 1e9f, smax = 0;
            std::vector<float> sc(h, 0.0f);
            for (unsigned y = 0; y < h; y++) {
                if (!m7row[y] || first + (int)y >= 240) continue;
                const int16_t *m = in.m7lines + 8 * (first + y);
                sc[y] = sqrtf((float)m[0] * m[0] + (float)m[2] * m[2]) / 256.0f;
                if (sc[y] > 0) { smin = std::min(smin, sc[y]); smax = std::max(smax, sc[y]); }
            }
            slope = smax > smin * 1.15f;
            const float R0 = 1.0f / (1.0f - 0.8f / 8.5f);  // distance ratio of disparity 1 (just behind the playfield)
            for (unsigned y = 0; y < h; y++)
                if (m7row[y] && slope && sc[y] > 0) floorD[y] = (1.0f - 1.0f / (R0 * sc[y] / smin)) * (8.5f / 0.8f);
            frameHasM7 = !(in.m7flags & 256);  // direct-colour Mode 7 keeps the sheet colours
        }
        const float horizonD = (m7top >= 0) ? floorD[m7top] : M7_FAR;
        float farD = 0;  // farthest floor line: the backdrop and skyline must stay behind it
        int floorZ = 39;
        for (unsigned y = 0; y < h; y++) if (m7row[y]) farD = std::max(farD, floorD[y]);
        if (m7top >= 0) for (unsigned x = 0; x < w; x++) if (in.layer[m7top * w + x] == 6) { floorZ = in.zmain[m7top * w + x]; break; }

        auto rowDisparity = [&](int n, int z, unsigned y) {
            if (y >= h) y = h - 1;
            if (n == 0 && m7row[y] && m7top >= 0) return floorD[y];
            float d = (n == 5) ? std::max(7.5f, farD + 1.0f) : (n == 0 && m7row[y]) ? 1.0f : disparityFor(z - 32, mode01);
            if (m7top >= 0 && (int)y < m7top && n != 4 && n != 5 && d > 0.0f) d = std::max(d, horizonD + 0.3f + 0.04f * d);  // skyline beyond the horizon
            // sprites on a sloping floor: just in front of it where they outrank it, just behind where they do not
            if (m7top >= 0 && n == 4 && m7row[y] && slope) d = z > floorZ ? std::min(d, floorD[y] - 0.3f) : floorD[y] + 0.3f;
            return d;
        };

        double t1 = nowMs();
        // one quad per row span of each (layer, priority) sheet: one pass per row finds every sheet's span
        for (int n = 0; n < 5; n++) {
            for (unsigned y = 0; y < h; y++) {
                const uint8_t *row = &in.planeZ[n][y * w];
                int16_t x0[256], x1[256];
                uint8_t used[256]; int nUsed = 0;
                for (unsigned x = 0; x < w; x++) {
                    uint8_t z = row[x];
                    if (!z) continue;
                    bool known = false;
                    for (int k = 0; k < nUsed; k++) if (used[k] == z) { known = true; break; }
                    if (!known) { used[nUsed++] = z; x0[z] = (int16_t)x; }
                    x1[z] = (int16_t)(x + 1);
                }
                std::sort(used, used + nUsed);
                for (int k = 0; k < nUsed; k++) {
                    int z = used[k];
                    Quad q{(float)x0[z], (float)x1[z], (float)y, rowDisparity(n, z, y), (float)n, (float)z};
                    bool floorRow = n == 0 && m7row[y] && m7top >= 0;
                    // a sloping floor is one continuous surface: the bottom edge meets the next line
                    q.disparity1 = (floorRow && y + 1 < h && m7row[y + 1]) ? rowDisparity(n, z, y + 1) : q.disparity;
                    if (n == 4 && slope && m7row[y]) q.disparity1 = (y + 1 < h && m7row[y + 1]) ? rowDisparity(n, z, y + 1) : q.disparity;
                    if (floorRow && frameHasM7) {
                        int first = in.m7flags >> 9, line = first + (int)y, next = (y + 1 < h && m7row[y + 1]) ? line + 1 : line;
                        q.m7 = true;
                        mode7uv(in.m7lines, in.m7flags, line, (float)x0[z], (float)line, q.m7uv[0][0], q.m7uv[0][1]);
                        mode7uv(in.m7lines, in.m7flags, line, (float)x1[z], (float)line, q.m7uv[1][0], q.m7uv[1][1]);
                        mode7uv(in.m7lines, in.m7flags, next, (float)x0[z], (float)line + 1, q.m7uv[2][0], q.m7uv[2][1]);
                        mode7uv(in.m7lines, in.m7flags, next, (float)x1[z], (float)line + 1, q.m7uv[3][0], q.m7uv[3][1]);
                    }
                    quads.push_back(q);
                }
            }
        }
        if (showBackdrop)
            for (unsigned y = 0; y < h; y++) {
                Quad q{0.0f, (float)w, (float)y, rowDisparity(5, 1, y), 5.0f, 1.0f};
                q.disparity1 = q.disparity;
                quads.push_back(q);
            }
        // sprite cards: each span gets a wall on its left and right edge, at the sprite face
        if (spriteWalls)
            for (const auto &q : quads)
                if ((int)q.slice == 4) {
                    walls.push_back({q.u0, q.v, q.v + 1.0f, q.disparity, q.disparity1});
                    walls.push_back({q.u1, q.v, q.v + 1.0f, q.disparity, q.disparity1});
                }
        double t2 = nowMs();
        if (frameHasM7) buildMap7(in);
        double t3 = nowMs();
        if (look.on) bakeLook(w, h);
        double t4 = nowMs();

        // far to near; equal depth keeps SNES priority order (higher z drawn later)
        std::stable_sort(quads.begin(), quads.end(), [](const Quad &a, const Quad &b) {
            if (a.disparity != b.disparity) return a.disparity > b.disparity;
            return a.z < b.z;
        });
        double t5 = nowMs();
        profMs[0] = t1 - t0; profMs[1] = t2 - t1; profMs[2] = t3 - t2; profMs[3] = t4 - t3; profMs[4] = t5 - t4;
    }

    void buildMap7(const Input &in) {
        uint64_t hsh = 1469598103934665603ull;
        auto mix = [&](const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { hsh ^= p[i]; hsh *= 1099511628211ull; } };
        mix(in.vram, 0x8000 * 2);
        mix((const uint8_t *)in.cgram, 512);
        int bright = (in.m7flags >> 4) & 15;
        hsh ^= (uint64_t)bright * 0x9e3779b97f4a7c15ull;
        m7flags = in.m7flags;
        if (hsh == map7Hash && !map7.empty()) return;
        map7Hash = hsh;
        map7.resize(1024 * 1024);
        uint32_t pal[256];
        for (int i = 0; i < 256; i++) {
            uint16_t c = in.cgram[i];
            uint32_t r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
            r = r * (bright + 1) / 16; g = g * (bright + 1) / 16; b = b * (bright + 1) / 16;
            pal[i] = (i ? 0xff000000u : 0u) | (b << 16) | (g << 8) | r;
        }
        for (int ty = 0; ty < 128; ty++)
            for (int tx = 0; tx < 128; tx++) {
                int tile = in.vram[(ty * 128 + tx) * 2];
                const uint8_t *td = in.vram + 1 + tile * 128;
                for (int py = 0; py < 8; py++)
                    for (int px = 0; px < 8; px++)
                        map7[(size_t)(ty * 8 + py) * 1024 + tx * 8 + px] = pal[td[(py * 8 + px) * 2]];
            }
        map7Dirty = true;
    }

    // nearest disparity at each screen position, padded by PAD so the shadow taps need no bounds checks
    static const int PAD = 4;
    std::vector<float> nearest;

    // Works straight from the quads (each sheet's row spans) instead of per-slice scratch buffers:
    // every sheet pixel is visited once to find what is nearest, then once to light it.
    void bakeLook(unsigned w, unsigned h) {
        const int pw = (int)w + PAD;
        nearest.assign((size_t)pw * (h + PAD), 1e9f);
        for (const auto &q : quads) {
            const uint32_t *row = slice((int)q.slice) + (size_t)q.v * TEX_W;
            float *nr = &nearest[(size_t)((int)q.v + PAD) * pw + PAD];
            const uint32_t z = (uint32_t)q.z;
            for (int x = (int)q.u0; x < (int)q.u1; x++)
                if ((row[x] >> 24) == z && q.disparity < nr[x]) nr[x] = q.disparity;
        }
        const float shadowK = look.shadow * 0.5f / 1.5f;
        for (const auto &q : quads) {
            const int s = (int)q.slice, y = (int)q.v;
            uint32_t *row = slice(s) + (size_t)y * TEX_W;
            const uint32_t *up = y > 0 ? row - TEX_W : nullptr, *down = y + 1 < (int)h ? row + TEX_W : nullptr;
            const float *n2 = &nearest[(size_t)(y - 2 + PAD) * pw + PAD - 2], *n4 = &nearest[(size_t)(y - 4 + PAD) * pw + PAD - 4];
            const uint32_t z = (uint32_t)q.z;
            const float d = q.disparity;
            const float hz = std::clamp(d * look.haze, 0.0f, 0.22f);
            const bool edges = s != 5;  // cut-paper edge on everything except the backdrop
            for (int x = (int)q.u0; x < (int)q.u1; x++) {
                uint32_t c = row[x];
                if ((c >> 24) != z) continue;
                // drop shadow: a nearer sheet up-left of here blocks the light (two taps = soft edge)
                float g1 = std::clamp(d - n2[x], 0.0f, 1.5f), g2 = std::clamp(d - n4[x], 0.0f, 1.5f);
                float k = 1.0f - shadowK * (g1 + g2);
                if (edges) {
                    bool l = x == 0 || (row[x - 1] >> 24) == z, u = !up || (up[x] >> 24) == z;
                    if (!l || !u) k += look.rim;
                    else {
                        bool r = x + 1 >= (int)w || (row[x + 1] >> 24) == z, dn = !down || (down[x] >> 24) == z;
                        if (!r || !dn) k -= look.rim;
                    }
                }
                float r = (float)(c & 255) * k, g = (float)((c >> 8) & 255) * k, b = (float)((c >> 16) & 255) * k;
                r += (150.0f - r) * hz; g += (170.0f - g) * hz; b += (200.0f - b) * hz;
                auto cl = [](float v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
                row[x] = cl(r) | (cl(g) << 8) | (cl(b) << 16) | (z << 24);
            }
        }
    }
};

}  // namespace diorama
