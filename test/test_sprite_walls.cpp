// Host-side tests for the sprite card walls in shared/diorama.h (Builder::thickness and Builder::walls).
// Walls are optional geometry: they must never change the sheets, the quads or the texture.
//
// Build and run:  clang++ -std=c++20 -I shared test/test_sprite_walls.cpp -o /tmp/test_sprite_walls_novel-sprite-thickness && /tmp/test_sprite_walls_novel-sprite-thickness

#include "diorama.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void check(const char *what, bool ok) {
    std::printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

const unsigned W = 6, H = 3;

// Backdrop everywhere (layer 5), one 2-pixel sprite span on row 1 at columns 2 and 3 (layer 4).
struct Scene {
    std::vector<uint16_t> rgb, color[5];
    std::vector<uint8_t> layer, zmain, z[5];
    Scene() : rgb(W * H, 0x4210), layer(W * H, 5), zmain(W * H, 0) {
        for (int n = 0; n < 5; n++) {
            color[n].assign(W * H, 0);
            z[n].assign(W * H, 0);
        }
        const uint8_t spriteZ = 42;  // OBJ priority 10
        for (unsigned x = 2; x < 4; x++) {
            size_t i = 1 * W + x;
            layer[i] = 4;
            zmain[i] = spriteZ;
            z[4][i] = spriteZ;
            color[4][i] = 0xF800;
            rgb[i] = 0xF800;
        }
    }
};

diorama::Input makeInput(Scene &s) {
    diorama::Input in{W, H, s.rgb.data(), s.layer.data(), s.zmain.data(), {}, {}};
    for (int n = 0; n < 5; n++) {
        in.planeColor[n] = s.color[n].data();
        in.planeZ[n] = s.z[n].data();
    }
    return in;
}

}  // namespace

int main() {
    Scene scene;
    diorama::Input in = makeInput(scene);

    auto flat = std::make_unique<diorama::Builder>();   // thickness 0
    auto card = std::make_unique<diorama::Builder>();   // thickness 2
    flat->thickness = 0.0f;
    card->thickness = 2.0f;
    flat->build(in);
    card->build(in);

    check("thickness 0: no walls", flat->walls.empty());
    check("thickness 2: one left and one right wall", card->walls.size() == 2);
    check("thickness 2: same quad count as thickness 0", card->quads.size() == flat->quads.size());

    bool quadsSame = card->quads.size() == flat->quads.size();
    for (size_t i = 0; quadsSame && i < flat->quads.size(); i++) {
        const diorama::Quad &a = flat->quads[i], &b = card->quads[i];
        quadsSame = a.u0 == b.u0 && a.u1 == b.u1 && a.v == b.v && a.disparity == b.disparity && a.slice == b.slice &&
                    a.z == b.z && a.disparity1 == b.disparity1 && a.m7 == b.m7;
    }
    check("thickness 2: quads identical to thickness 0", quadsSame);
    check("thickness 2: texture identical to thickness 0", card->tex == flat->tex);

    // the one sprite span is row 1, columns [2, 4)
    const diorama::Quad *sprite = nullptr;
    int sprites = 0;
    for (const auto &q : card->quads)
        if ((int)q.slice == 4) { sprite = &q; sprites++; }
    check("sprite span is one quad at row 1, columns [2, 4)",
          sprites == 1 && sprite && sprite->u0 == 2.0f && sprite->u1 == 4.0f && sprite->v == 1.0f);

    if (sprite && card->walls.size() == 2) {
        const diorama::Wall &a = card->walls[0], &b = card->walls[1];
        const diorama::Wall &left = a.u == 2.0f ? a : b, &right = a.u == 2.0f ? b : a;
        check("left wall sits on the span start (u = 2)", a.u == 2.0f || b.u == 2.0f);
        check("right wall sits on the span end (u = 4)", a.u == 4.0f || b.u == 4.0f);
        check("walls cover row 1 only (v1 == v0 + 1)", left.v0 == 1.0f && left.v1 == left.v0 + 1.0f &&
                                                       right.v0 == 1.0f && right.v1 == right.v0 + 1.0f);
        check("walls start at the sprite face (dFront == span disparity)",
              left.dFront == sprite->disparity && right.dFront == sprite->disparity);
        check("walls end 2 SNES px behind the face (dBack == dFront + 2)",
              left.dBack == left.dFront + 2.0f && right.dBack == right.dFront + 2.0f);
    } else {
        check("walls cover row 1 only (v1 == v0 + 1)", false);
        check("walls start at the sprite face (dFront == span disparity)", false);
        check("walls end 2 SNES px behind the face (dBack == dFront + 2)", false);
    }

    card->thickness = 0.0f;
    card->build(in);
    check("rebuild with thickness 0 clears the walls", card->walls.empty());

    std::printf("%s\n", failures ? "FAILED" : "all sprite wall tests passed");
    return failures ? 1 : 0;
}
