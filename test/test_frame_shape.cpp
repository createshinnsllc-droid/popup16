// Host-side tests for shared/frame_shape.h, the rule that stops the clip encoder from converting a
// frame whose dimensions no longer fit its NV12 buffer.
//
// Build and run:  c++ -std=c++20 -O2 -Wall -Wextra -o /tmp/test_frame_shape test/test_frame_shape.cpp && /tmp/test_frame_shape

#include "../shared/frame_shape.h"

#include <cstdio>
#include <deque>
#include <vector>

namespace {

// Mirrors the recorder's Frame: a shape plus a payload we deliberately ignore here.
struct FakeFrame {
    uint16_t w, h;
    unsigned payload;
};

int failures = 0;

template <class Fn>
void check(const char *what, Fn fn, bool expected) {
    bool got = fn();
    bool ok = got == expected;
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

}  // namespace

int main() {
    using frames::Shape;
    using frames::sameShape;
    using frames::allMatchFirst;

    check("equal shapes compare equal", [] { return sameShape({256, 224}, {256, 224}); }, true);
    check("different width compares unequal", [] { return sameShape({256, 224}, {512, 224}); }, false);
    check("different height compares unequal", [] { return sameShape({256, 224}, {256, 448}); }, false);

    check("uniform 256x224 frames are accepted", [] {
        std::vector<FakeFrame> f{{256, 224, 1}, {256, 224, 2}, {256, 224, 3}};
        return allMatchFirst(f);
    }, true);

    check("a single frame is accepted", [] {
        std::vector<FakeFrame> f{{256, 224, 1}};
        return allMatchFirst(f);
    }, true);

    check("empty recording is refused, not treated as uniform", [] {
        std::vector<FakeFrame> f;
        return allMatchFirst(f);
    }, false);

    check("width increase mid-recording is refused", [] {
        std::deque<FakeFrame> f{{256, 224, 1}, {256, 224, 2}, {512, 224, 3}};
        return allMatchFirst(f);
    }, false);

    check("height increase mid-recording is refused (the hires case)", [] {
        std::deque<FakeFrame> f{{256, 224, 1}, {256, 448, 2}};
        return allMatchFirst(f);
    }, false);

    check("change on the very first frame is refused", [] {
        std::deque<FakeFrame> f{{512, 448, 1}, {256, 224, 2}};
        return allMatchFirst(f);
    }, false);

    // The old code compared byte counts in one path, so a same-area swap could slip through.
    check("equal-area dimension swap is still refused", [] {
        std::deque<FakeFrame> f{{512, 224, 1}, {256, 448, 2}};
        return allMatchFirst(f);
    }, false);

    check("a later return to the original size is still refused", [] {
        std::deque<FakeFrame> f{{256, 224, 1}, {512, 448, 2}, {256, 224, 3}};
        return allMatchFirst(f);
    }, false);

    if (failures) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
