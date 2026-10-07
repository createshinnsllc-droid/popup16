// Frame-shape rule shared by the clip recorder.
//
// The encoder and its NV12 scratch buffer are sized once, from the first recorded frame. Every later
// frame in the same clip must therefore have exactly the same width and height. A mid-recording
// resolution change (entering a hires mode, switching game, a mode that alternates tall/short
// frames) used to be converted using its own dimensions, which could write past the end of that
// buffer. Monotonic recording is now a precondition the caller must check.
//
// Kept free of any Android dependency on purpose, so the rule can be unit tested on a desktop host.
#pragma once
#include <cstdint>

namespace frames {

struct Shape {
    uint16_t w = 0, h = 0;
};

inline bool sameShape(Shape a, Shape b) { return a.w == b.w && a.h == b.h; }

// True when every frame matches the first. False for an empty range, because there is nothing to
// encode and callers must not treat "no frames" as a valid uniform shape.
template <class Container>
inline bool allMatchFirst(const Container &list) {
    if (list.empty()) return false;
    for (const auto &f : list)
        if (f.w != list.front().w || f.h != list.front().h) return false;
    return true;
}

}  // namespace frames
