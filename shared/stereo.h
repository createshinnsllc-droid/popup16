// Shared SNES stereo model: per-layer disparity + per-eye forward warp.
// Input: a frame from the patched snes9x core (RGB565 pixels, layer id and priority depth per pixel).
// Output: left and right eye images side by side, EYE_W pixels wide each, rows repeated vertically.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace stereo {

static const int SUB = 4;               // horizontal subpixels per SNES pixel (256 -> 1024 per eye)
static const int EYE_W = 256 * SUB;
static const float M7_FAR = 7.0f;       // Mode 7 floor disparity at the horizon

struct Params {
    float strength = 1.0f;     // depth multiplier
    float convergence = 0.0f;  // added to every disparity (SNES px); + pushes scene into the screen
    float maxFar = 10.0f;      // clamp for uncrossed disparity (keeps eyes from diverging)
    float maxNear = 6.0f;      // clamp for crossed disparity
    bool enabled = true;
    bool mode7Ramp = true;
    bool swapEyes = false;
};

struct Frame {
    unsigned w = 256, h = 224;
    std::vector<uint16_t> rgb565;
    std::vector<uint8_t> layers, depths;
    bool valid = false;

    // pitch in bytes; layer / depth buffers share the pixel layout (pitch/2 bytes per row)
    void capture(const void *data, unsigned fw, unsigned fh, size_t pitch, const uint8_t *L, const uint8_t *Z) {
        w = fw; h = fh;
        rgb565.resize(w * h); layers.resize(w * h); depths.resize(w * h);
        size_t ppl = pitch / 2;
        for (unsigned y = 0; y < h; y++) {
            memcpy(&rgb565[y * w], (const uint8_t *)data + y * pitch, w * 2);
            if (L) memcpy(&layers[y * w], L + y * ppl, w); else memset(&layers[y * w], 0, w);
            if (Z) memcpy(&depths[y * w], Z + y * ppl, w); else memset(&depths[y * w], 32 + 15, w);
        }
        valid = true;
    }
};

// Layer ids from the patched core: 0-3 BG1-BG4, 4 sprites, 5 backdrop, 6 Mode 7 BG1, 7 Mode 7 EXTBG.
// z is the snes9x priority depth (32 + per-layer value on the main screen).
// Returns disparity in SNES pixels; positive = behind the screen plane.
inline float baseDisparity(int layer, int z) {
    int p = z - 32;
    switch (layer) {
    case 0: return p == 15 ? 0.0f : 1.0f;                     // main playfield
    case 1: return (p == 14 || p == 11) ? 2.0f : 3.0f;        // mid background
    case 2:
        if (p == 17) return -1.5f;                            // Mode 1 high-priority BG3: status bar / HUD
        return p == 7 ? 4.0f : 5.0f;                          // far background / clouds
    case 3: return p == 6 ? 5.5f : 6.0f;
    case 4: {                                                 // sprites, by OAM priority 0-3
        int pri = std::clamp((p - 4) / 4, 0, 3);
        static const float d[4] = {2.0f, 1.0f, -0.5f, -1.0f};
        return d[pri];
    }
    case 5: return 7.0f;                                      // backdrop
    case 7: return 2.0f;
    default: return 0.0f;
    }
}

// lut maps RGB565 to the caller's 32-bit pixel format.
// out is resized to (2*EYE_W) x outH; returns outH.
inline unsigned build(const Frame &f, const Params &p, const uint32_t *lut, std::vector<uint32_t> &out) {
    const unsigned fw = f.w, fh = f.h;
    unsigned vrep = fh <= 240 ? 4 : 2;
    unsigned outH = fh * vrep;
    out.resize((size_t)2 * EYE_W * outH);

    int m7top = -1;  // Mode 7 horizon: first row containing Mode 7 pixels
    if (p.mode7Ramp)
        for (unsigned y = 0; y < fh && m7top < 0; y++)
            for (unsigned x = 0; x < fw; x++)
                if (f.layers[y * fw + x] == 6) { m7top = (int)y; break; }

    int sub = std::max(1, (int)(EYE_W / fw));  // 4 for 256 wide, 2 for hires 512
    std::vector<float> disp(fw), zb(EYE_W);
    std::vector<uint32_t> row(EYE_W);
    for (unsigned y = 0; y < fh; y++) {
        float m7d = 0.0f;
        if (m7top >= 0) {
            float t = (float)((int)y - m7top) / std::max(1.0f, (float)(fh - 1 - m7top));
            m7d = (M7_FAR + 1.0f) * (1.0f - t) - 1.0f;  // far at the horizon, slightly in front at the bottom
        }
        for (unsigned x = 0; x < fw; x++) {
            int l = f.layers[y * fw + x];
            float d = (l == 6) ? (p.mode7Ramp ? m7d : 0.0f) : baseDisparity(l, f.depths[y * fw + x]);
            // perspective Mode 7 scene: scenery above the floor (skyline, sky) sits beyond the horizon
            if (m7top >= 0 && (int)y < m7top && l != 4 && d > 0.0f) d = M7_FAR + 0.5f * d;
            d = p.enabled ? d * p.strength + p.convergence : 0.0f;
            disp[x] = std::clamp(d, -p.maxNear, p.maxFar);
        }
        const uint16_t *src = &f.rgb565[y * fw];
        for (int eye = 0; eye < 2; eye++) {
            int sign = (eye == 0) ? -1 : 1;  // left eye: far things shift left
            if (p.swapEyes) sign = -sign;
            std::fill(zb.begin(), zb.end(), INFINITY);
            for (unsigned x = 0; x < fw; x++) {
                float d = disp[x];
                int ox0 = (int)x * sub + (int)lrintf(sign * d * 0.5f * SUB);
                uint32_t c = lut[src[x]];
                for (int k = 0; k < sub; k++) {
                    int ox = ox0 + k;
                    if (ox < 0 || ox >= EYE_W) continue;
                    if (d <= zb[ox]) { zb[ox] = d; row[ox] = c; }  // nearer wins
                }
            }
            // fill disocclusion holes from the farther neighbour
            int x = 0;
            while (x < EYE_W) {
                if (zb[x] != INFINITY) { x++; continue; }
                int a = x;
                while (x < EYE_W && zb[x] == INFINITY) x++;
                int b = x;
                uint32_t c = lut[0];
                if (a > 0 && b < EYE_W) c = zb[a - 1] >= zb[b] ? row[a - 1] : row[b];
                else if (a > 0) c = row[a - 1];
                else if (b < EYE_W) c = row[b];
                for (int i = a; i < b; i++) row[i] = c;
            }
            for (unsigned r = 0; r < vrep; r++)
                memcpy(&out[((size_t)(y * vrep + r) * 2 * EYE_W) + eye * EYE_W], row.data(), EYE_W * 4);
        }
    }
    return outH;
}

// 565 -> 32-bit tables. argb: 0xAARRGGBB (SDL ARGB8888). rgba: bytes R,G,B,A in memory (GL_RGBA).
inline void makeLut(uint32_t *lut, bool rgbaBytes) {
    for (int i = 0; i < 65536; i++) {
        uint32_t r = ((i >> 11) & 31) * 255 / 31, g = ((i >> 5) & 63) * 255 / 63, b = (i & 31) * 255 / 31;
        lut[i] = rgbaBytes ? (0xff000000u | (b << 16) | (g << 8) | r) : (0xff000000u | (r << 16) | (g << 8) | b);
    }
}

}  // namespace stereo
