// Small pure geometry helpers for the atelier interface.
//
// Everything here is deliberately free of Android and OpenXR types so the rules can be unit tested
// on a desktop host. Two problems live here:
//
//   1. fit     - cover art arrives in whatever shape the user's file happens to be. Squashing it to
//                the card's aspect distorts faces and text, so it is scaled to fit and centred.
//   2. seat    - window/box depth used to be the distance from the placement to the LOCAL origin,
//                which goes stale the moment the user recenters or sits somewhere new. Depth is
//                measured from an explicit seat instead: the viewer position the screen was placed
//                for. Recomputing it per frame from the live head pose would erase parallax, so the
//                seat is a fixed pose that only changes when the user marks it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace place {

struct Size {
    int w = 0, h = 0;
};
struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Scale src down (never up) to the largest size that fits inside box, preserving aspect ratio.
// Returns a zero size when any input is degenerate, so callers can fall back to a placeholder.
inline Size fitInside(Size src, Size box) {
    if (src.w <= 0 || src.h <= 0 || box.w <= 0 || box.h <= 0) return {};
    const float scale = std::min((float)box.w / (float)src.w, (float)box.h / (float)src.h);
    const float scaled = std::min(scale, 1.0f);
    Size out{(int)std::lround(src.w * scaled), (int)std::lround(src.h * scaled)};
    // Rounding can land a pixel over; clamp so a fitted image can never overrun its card.
    out.w = std::min(out.w, box.w);
    out.h = std::min(out.h, box.h);
    if (out.w <= 0 || out.h <= 0) return {};
    return out;
}

// Top-left corner that centres inner within outer, never negative.
inline Size centreInside(Size inner, Size outer) {
    return Size{std::max(0, (outer.w - inner.w) / 2), std::max(0, (outer.h - inner.h) / 2)};
}

inline float distance(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Distance from the marked seat to the placement, clamped to a sane minimum so a placement at the
// seat itself can never collapse the projection or divide by zero.
inline float seatDepth(Vec3 placement, Vec3 seat, float minimum) {
    return std::max(minimum, distance(placement, seat));
}

}  // namespace place
