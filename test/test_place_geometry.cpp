// Host-side tests for shared/place_geometry.h.
//
//   c++ -std=c++20 -O2 -Wall -Wextra -o /tmp/test_place test/test_place_geometry.cpp && /tmp/test_place

#include "../shared/place_geometry.h"

#include <cmath>
#include <cstdio>

using place::Size;
using place::Vec3;
using place::fitInside;
using place::centreInside;
using place::distance;
using place::seatDepth;

namespace {
int failures = 0;

void expectSize(const char *what, Size got, Size want) {
    const bool ok = got.w == want.w && got.h == want.h;
    std::printf("%-56s %s  (got %dx%d)\n", what, ok ? "ok" : "FAIL", got.w, got.h);
    if (!ok) ++failures;
}
void expectNear(const char *what, float got, float want, float tol = 1e-4f) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%-56s %s  (got %.4f)\n", what, ok ? "ok" : "FAIL", got);
    if (!ok) ++failures;
}
void expectTrue(const char *what, bool got) {
    std::printf("%-56s %s\n", what, got ? "ok" : "FAIL");
    if (!got) ++failures;
}
}  // namespace

int main() {
    // ---- cover fitting: the card is 224x196, most SNES art is 4:3 or square ----
    expectSize("4:3 art fits the card without touching width", fitInside({256, 192}, {224, 196}), {224, 168});
    expectSize("square art is pillarboxed, not stretched", fitInside({256, 256}, {224, 196}), {196, 196});
    expectSize("tall art is pillarboxed", fitInside({200, 400}, {224, 196}), {98, 196});
    expectSize("wide art is letterboxed", fitInside({800, 200}, {224, 196}), {224, 56});
    expectSize("art already card-sized is untouched", fitInside({224, 196}, {224, 196}), {224, 196});
    expectSize("small art is never upscaled into a blurry mess", fitInside({100, 75}, {224, 196}), {100, 75});
    expectSize("degenerate source yields no image", fitInside({0, 0}, {224, 196}), {0, 0});
    expectSize("degenerate box yields no image", fitInside({256, 192}, {0, 0}), {0, 0});
    expectTrue("no fitted image ever exceeds the card box", [] {
        const Size box{224, 196};
        const int dims[][2] = {{1, 1}, {3, 2000}, {2000, 3}, {224, 196}, {223, 197}, {4096, 4096}, {16, 9}};
        for (const auto &d : dims) {
            const Size got = fitInside({d[0], d[1]}, box);
            if (got.w > box.w || got.h > box.h) return false;
            // An extreme aspect ratio can round away to nothing, which callers treat as "no art".
            // That is allowed, but a half-formed size is not.
            if ((got.w == 0) != (got.h == 0)) return false;
        }
        return true;
    }());
    expectSize("an extreme aspect ratio rounds away to no art", fitInside({3, 2000}, {224, 196}), {0, 0});

    expectSize("fitted 4:3 art is centred with equal side bars", centreInside({224, 168}, {224, 196}), {0, 14});
    expectSize("fitted square art is centred horizontally", centreInside({196, 196}, {224, 196}), {14, 0});
    expectSize("an exact fit has no bars", centreInside({224, 196}, {224, 196}), {0, 0});

    // ---- seat depth ----
    expectNear("depth is the plain distance from the seat", seatDepth({0, 0, -2.2f}, {0, 0, 0}, 0.4f), 2.2f);
    expectNear("a placement at the seat clamps to the minimum", seatDepth({1, 1, 1}, {1, 1, 1}, 0.4f), 0.4f);
    expectNear("a nearer placement clamps rather than inverting", seatDepth({0, 0, 0.1f}, {0, 0, 0}, 0.4f), 0.4f);
    expectNear("distance is symmetric", distance({1, 2, 3}, {4, 6, 3}), 5.0f);

    // The regression this fix exists for: the user recenters or sits somewhere new, so their seat
    // shifts. Depth must follow the seat, not keep the stale number measured from the room origin.
    // A screen 2.2 m in front and 1.6 m up, seen from a seat 1.6 m closer than the origin.
    const Vec3 placement{0.0f, 1.6f, -2.2f};
    const Vec3 oldSeat{0.0f, 0.0f, 0.0f};      // room origin: what the old code always assumed
    const Vec3 newSeat{0.0f, 1.6f, -0.6f};     // after a recenter / sitting somewhere new
    expectNear("depth from the room origin", seatDepth(placement, oldSeat, 0.4f), 2.720294f, 1e-4f);
    expectNear("depth from the real seat", seatDepth(placement, newSeat, 0.4f), 1.600000f, 1e-4f);
    expectTrue("a shifted seat really does change the depth used", [] {
        const Vec3 p{0.0f, 1.6f, -2.2f};
        const float stale = seatDepth(p, {0, 0, 0}, 0.4f);
        const float real = seatDepth(p, {0, 1.6f, -0.6f}, 0.4f);
        return std::fabs(stale - real) > 1.0f;
    }());
    expectTrue("marking a seat leaves classic behaviour untouched when it is at the origin", [] {
        const Vec3 p{0.3f, 1.5f, -1.8f};
        return seatDepth(p, {0, 0, 0}, 0.4f) == std::max(0.4f, std::sqrt(0.09f + 2.25f + 3.24f));
    }());

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
