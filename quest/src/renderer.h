// Diorama renderer: draws the SNES layer sheets (shared/diorama.h) as real geometry into the two
// eye images of an OpenXR projection layer. The whole diorama has a placement (uModel) in the room.
// Screen style: each sheet sits at the distance its disparity implies and is scaled so it covers the
// screen exactly from the seat in front of it. Box style: same-size sheets stacked behind a front
// plane, like a paper diorama you can walk around.
#pragma once
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <cmath>
#include <vector>
#include "../../shared/diorama.h"

namespace render {

static const char *kVS = R"(#version 300 es
layout(location = 0) in vec2 aPos;      // texel column, row
layout(location = 1) in float aDisp;    // disparity in SNES pixels
layout(location = 2) in vec2 aSheet;    // slice, priority depth
layout(location = 3) in vec3 aM7;       // HD Mode 7 map texel coords, flag (1 = floor)
uniform mat4 uViewProj;
uniform vec4 uScreen;                   // width m, height m, distance m, metres per SNES pixel
uniform vec4 uDepth;                    // ipd m, disparity scale, disparity offset, unused
uniform vec2 uFrame;                    // frame width, height in texels
uniform mat4 uModel;                    // placement: screen centre and orientation in the room
uniform vec2 uStyle;                    // x: 0 screen, 1 box; y: box depth in metres per SNES pixel
out vec2 vTex;
flat out vec2 vSheet;
out vec3 vM7;
out float vDisp;
void main() {
    float d = aDisp * uDepth.y + uDepth.z;
    float ipd = uDepth.x;
    float s = clamp(d * uScreen.w, -2.0 * ipd, 0.96 * ipd);  // on-screen eye shift, never diverging (25x screen distance max)
    float dist = uScreen.z * ipd / (ipd - s);
    vec2 xy = vec2((aPos.x / uFrame.x - 0.5) * uScreen.x, (0.5 - aPos.y / uFrame.y) * uScreen.y);
    vec3 p = uStyle.x < 0.5 ? vec3(xy * (dist / uScreen.z), uScreen.z - dist)   // seen from (0, 0, distance)
                            : vec3(xy, -d * uStyle.y);
    gl_Position = uViewProj * (uModel * vec4(p, 1.0));
    vTex = aPos;
    vSheet = aSheet;
    vM7 = aM7;
    vDisp = aDisp;
}
)";

static const char *kFS = R"(#version 300 es
precision highp float;
precision highp sampler2DArray;
uniform sampler2DArray uTex;
uniform sampler2D uMap;                 // 1024x1024 Mode 7 map, mipmapped
uniform vec2 uFrame;
uniform float uHaze;                    // pop-up look haze per SNES pixel of depth (0 = off)
in vec2 vTex;
flat in vec2 vSheet;
in vec3 vM7;
in float vDisp;
out vec4 oColor;
void main() {
    ivec2 t = clamp(ivec2(floor(vTex)), ivec2(0), ivec2(uFrame) - 1);
    vec4 c = texelFetch(uTex, ivec3(t, int(vSheet.x + 0.5)), 0);
    if (abs(c.a * 255.0 - vSheet.y) > 0.5) discard;
    vec3 rgb = c.rgb;
    if (vM7.z > 0.5) {  // HD floor: sample the game's own map at full resolution
        vec4 m = texture(uMap, vM7.xy / 1024.0);
        if (m.a > 0.5) {
            float hz = clamp(vDisp * uHaze, 0.0, 0.22);
            rgb = mix(m.rgb, vec3(0.30, 0.40, 0.58), hz);   // haze target in linear light
        }
    }
    oColor = vec4(rgb, 1.0);
}
)";

// flat-colour geometry (pointer rays, the grab bar) in room coordinates
static const char *kFlatVS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";
static const char *kFlatFS = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; }
)";

struct FlatShape { std::vector<float> tris; float color[4]; };  // x y z per vertex, triangles

struct Eye {
    XrSwapchain swap = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    GLuint depth = 0;
};

struct Renderer {
    GLuint prog = 0, tex = 0, vao = 0, vbo = 0, ibo = 0, map = 0;
    GLint uViewProj, uScreen, uDepth, uFrame, uTex, uModel, uStyle, uMap, uHaze;
    float haze = 0.0f;
    GLuint flatProg = 0, flatVao = 0, flatVbo = 0;
    GLint fViewProj = -1, fColor = -1;
    std::vector<FlatShape> shapes;  // drawn after the sheets each frame
    std::vector<float> opening;     // window style: the frame opening (triangles); sheets show only through it
    std::vector<FlatShape> inside;  // window style: the box walls behind the opening, clipped like the sheets
    std::vector<diorama::Wall> walls;  // sprite card edges of the shown build (copied at upload)
    std::vector<float> wallTris;       // box style: those walls as room-space triangles, reused each frame
    bool captureEye = false;        // debug: read back the next left-eye image
    std::vector<uint8_t> eyePixels; int eyeW = 0, eyeH = 0;
    int indexCount = 0;
    float frameW = 256, frameH = 224;
    Eye eyes[2];

    static GLuint compile(GLenum type, const char *src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    }
    bool init() {
        prog = glCreateProgram();
        glAttachShader(prog, compile(GL_VERTEX_SHADER, kVS));
        glAttachShader(prog, compile(GL_FRAGMENT_SHADER, kFS));
        glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) return false;
        uViewProj = glGetUniformLocation(prog, "uViewProj");
        flatProg = glCreateProgram();
        glAttachShader(flatProg, compile(GL_VERTEX_SHADER, kFlatVS));
        glAttachShader(flatProg, compile(GL_FRAGMENT_SHADER, kFlatFS));
        glLinkProgram(flatProg);
        fViewProj = glGetUniformLocation(flatProg, "uViewProj");
        fColor = glGetUniformLocation(flatProg, "uColor");
        glGenVertexArrays(1, &flatVao);
        glGenBuffers(1, &flatVbo);
        glBindVertexArray(flatVao);
        glBindBuffer(GL_ARRAY_BUFFER, flatVbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
        glBindVertexArray(0);
        uScreen = glGetUniformLocation(prog, "uScreen");
        uDepth = glGetUniformLocation(prog, "uDepth");
        uFrame = glGetUniformLocation(prog, "uFrame");
        uTex = glGetUniformLocation(prog, "uTex");
        uModel = glGetUniformLocation(prog, "uModel");
        uStyle = glGetUniformLocation(prog, "uStyle");
        uMap = glGetUniformLocation(prog, "uMap");
        uHaze = glGetUniformLocation(prog, "uHaze");
        glGenTextures(1, &map);
        glBindTexture(GL_TEXTURE_2D, map);
        glTexStorage2D(GL_TEXTURE_2D, 11, GL_SRGB8_ALPHA8, 1024, 1024);  // 1024 down to 1x1
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);  // crisp pixels up close

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        // sRGB storage: texels decode to linear, the sRGB eye images encode back (alpha stays raw)
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_SRGB8_ALPHA8, diorama::TEX_W, diorama::TEX_H, diorama::SLICES);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        const int stride = 8 * 4;  // x y disparity slice z u7 v7 floor
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void *)8);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *)12);
        glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void *)20);
        glBindVertexArray(0);
        return true;
    }

    // upload the sheet textures and geometry for a new frame
    void upload(diorama::Builder &b, unsigned w, unsigned h) {
        frameW = (float)w; frameH = (float)h;
        walls.assign(b.walls.begin(), b.walls.end());  // copied: the builder may be reused once uploaded
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, diorama::TEX_W);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, diorama::TEX_H);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, w, h, diorama::SLICES, GL_RGBA, GL_UNSIGNED_BYTE, b.tex.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);

        static std::vector<float> v;
        static std::vector<uint32_t> idx;
        v.clear(); idx.clear();
        if (b.map7Dirty && !b.map7.empty()) {  // the game changed its Mode 7 map or palette
            glBindTexture(GL_TEXTURE_2D, map);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1024, GL_RGBA, GL_UNSIGNED_BYTE, b.map7.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            b.map7Dirty = false;
        }
        {   // repeat mode 0 wraps the map; other modes show nothing (or tile 0) outside it
            GLint wrap = ((b.m7flags >> 2) & 3) == 0 ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            glBindTexture(GL_TEXTURE_2D, map);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        }
        for (const auto &q : b.quads) {
            uint32_t base = (uint32_t)(v.size() / 8);
            const float corners[4][2] = {{q.u0, q.v}, {q.u1, q.v}, {q.u0, q.v + 1}, {q.u1, q.v + 1}};
            for (int k = 0; k < 4; k++) {
                float d = k < 2 ? q.disparity : q.disparity1;
                v.insert(v.end(), {corners[k][0], corners[k][1], d, q.slice, q.z,
                                   q.m7 ? q.m7uv[k][0] : 0.0f, q.m7 ? q.m7uv[k][1] : 0.0f, q.m7 ? 1.0f : 0.0f});
            }
            idx.insert(idx.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STREAM_DRAW);
        indexCount = (int)idx.size();
    }

    // Sprite card thickness in metres: the back edge of each wall sits this far behind the sprite face,
    // in model space (before the placement matrix), so it does not depend on screen width or strength.
    static constexpr float WALL_THICKNESS_M = 0.004f;

    // Box style with pop-up look: each sprite card's edges as one flat dark shape (two triangles per wall).
    // Vertices use the sheet vertex shader's box mapping (screen position, depth from disparity), then the
    // placement matrix, so the walls line up with the sheets. Call once per frame, after shapes.clear().
    void pushWalls(const float screen[4], const float depth[4], const float model[16], const float style[2]) {
        if (walls.empty()) return;
        wallTris.clear();
        // dz: offset along model z from the sprite face (0 = front edge, -WALL_THICKNESS_M = back edge)
        auto put = [&](float u, float v, float dWall, float dz) {
            float d = dWall * depth[1] + depth[2];
            float p[3] = {(u / frameW - 0.5f) * screen[0], (0.5f - v / frameH) * screen[1], -d * style[1] + dz};
            for (int r = 0; r < 3; r++) wallTris.push_back(model[r] * p[0] + model[4 + r] * p[1] + model[8 + r] * p[2] + model[12 + r]);
        };
        for (const auto &w : walls) {
            // one quad: front and back edges over rows v0..v1 at column u
            put(w.u, w.v0, w.dFront, 0.0f); put(w.u, w.v1, w.dFront, 0.0f); put(w.u, w.v0, w.dFront, -WALL_THICKNESS_M);
            put(w.u, w.v1, w.dFront, 0.0f); put(w.u, w.v1, w.dFront, -WALL_THICKNESS_M); put(w.u, w.v0, w.dFront, -WALL_THICKNESS_M);
        }
        shapes.push_back(FlatShape{wallTris, {0.20f, 0.17f, 0.15f, 1.0f}});  // dark warm card edge
    }

    static void viewProj(const XrPosef &pose, const XrFovf &fov, float *m) {
        const float n = 0.05f, f = 200.0f;
        float l = tanf(fov.angleLeft), r = tanf(fov.angleRight), u = tanf(fov.angleUp), d = tanf(fov.angleDown);
        float P[16] = {2 / (r - l), 0, 0, 0, 0, 2 / (u - d), 0, 0, (r + l) / (r - l), (u + d) / (u - d), -(f + n) / (f - n), -1,
                       0, 0, -2 * f * n / (f - n), 0};
        const XrQuaternionf &q = pose.orientation;
        float x = q.x, y = q.y, z = q.z, w = q.w;
        float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                      2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                      2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};  // columns of the rotation
        const XrVector3f &t = pose.position;
        // view = inverse(pose): rotation transposed, translation -R^T t
        float V[16] = {R[0], R[3], R[6], 0, R[1], R[4], R[7], 0, R[2], R[5], R[8], 0,
                       -(R[0] * t.x + R[1] * t.y + R[2] * t.z), -(R[3] * t.x + R[4] * t.y + R[5] * t.z),
                       -(R[6] * t.x + R[7] * t.y + R[8] * t.z), 1};
        for (int c = 0; c < 4; c++)
            for (int r2 = 0; r2 < 4; r2++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += P[k * 4 + r2] * V[c * 4 + k];
                m[c * 4 + r2] = s;
            }
    }

    // draw into the acquired swapchain image of one eye
    // model: column-major placement matrix; style: {0 screen | 1 box, box metres per SNES pixel};
    // clearAlpha 0 lets the passthrough room show wherever no sheet is drawn
    void drawEye(int eye, uint32_t imageIndex, const XrView &view, const float screen[4], const float depth[4],
                 const float model[16], const float style[2], float clearAlpha, bool show) {
        Eye &e = eyes[eye];
        if (e.fbos[imageIndex] == 0) {
            glGenFramebuffers(1, &e.fbos[imageIndex]);
            glBindFramebuffer(GL_FRAMEBUFFER, e.fbos[imageIndex]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.images[imageIndex].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, e.depth);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, e.fbos[imageIndex]);
        glViewport(0, 0, e.w, e.h);
        glClearColor(0, 0, 0, clearAlpha);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (show && indexCount) {
            float m[16];
            viewProj(view.pose, view.fov, m);
            bool clip = !opening.empty();
            if (clip) {  // mark the window opening in the stencil buffer; the world is drawn only there
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(GL_ALWAYS, 1, 0xff);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);
                glDisable(GL_DEPTH_TEST);
                glUseProgram(flatProg);
                glUniformMatrix4fv(fViewProj, 1, GL_FALSE, m);
                glBindVertexArray(flatVao);
                glBindBuffer(GL_ARRAY_BUFFER, flatVbo);
                glBufferData(GL_ARRAY_BUFFER, opening.size() * 4, opening.data(), GL_STREAM_DRAW);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(opening.size() / 3));
                glBindVertexArray(0);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glStencilFunc(GL_EQUAL, 1, 0xff);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            }
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);  // equal depth: later (higher SNES priority) sheet wins
            glDisable(GL_CULL_FACE);
            glUseProgram(prog);
            glUniformMatrix4fv(uViewProj, 1, GL_FALSE, m);
            glUniform4fv(uScreen, 1, screen);
            glUniform4fv(uDepth, 1, depth);
            glUniform2f(uFrame, frameW, frameH);
            glUniformMatrix4fv(uModel, 1, GL_FALSE, model);
            glUniform2fv(uStyle, 1, style);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
            glUniform1i(uTex, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, map);
            glUniform1i(uMap, 1);
            glUniform1f(uHaze, haze);
            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(vao);
            glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(0);
            if (clip && !inside.empty()) {  // walls of the box, still only through the opening
                glUseProgram(flatProg);
                glUniformMatrix4fv(fViewProj, 1, GL_FALSE, m);
                glBindVertexArray(flatVao);
                glBindBuffer(GL_ARRAY_BUFFER, flatVbo);
                for (const auto &sh : inside) {
                    glBufferData(GL_ARRAY_BUFFER, sh.tris.size() * 4, sh.tris.data(), GL_STREAM_DRAW);
                    glUniform4fv(fColor, 1, sh.color);
                    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(sh.tris.size() / 3));
                }
                glBindVertexArray(0);
            }
            glDisable(GL_STENCIL_TEST);
        }
        if (!shapes.empty()) {  // pointer rays and the grab bar, in room space
            float m[16];
            viewProj(view.pose, view.fov, m);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glUseProgram(flatProg);
            glUniformMatrix4fv(fViewProj, 1, GL_FALSE, m);
            glBindVertexArray(flatVao);
            glBindBuffer(GL_ARRAY_BUFFER, flatVbo);
            for (const auto &sh : shapes) {
                glBufferData(GL_ARRAY_BUFFER, sh.tris.size() * 4, sh.tris.data(), GL_STREAM_DRAW);
                glUniform4fv(fColor, 1, sh.color);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(sh.tris.size() / 3));
            }
            glBindVertexArray(0);
        }
        if (captureEye && eye == 0) {
            eyePixels.resize((size_t)e.w * e.h * 4); eyeW = e.w; eyeH = e.h;
            glReadPixels(0, 0, e.w, e.h, GL_RGBA, GL_UNSIGNED_BYTE, eyePixels.data());
            captureEye = false;
        }
        const GLenum discardDepth = GL_DEPTH_STENCIL_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &discardDepth);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

}  // namespace render
