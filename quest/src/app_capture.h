// PopUp16 Quest app: capture: screenshots and clips.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- capture: screenshots and clips
// Files go to the headset's shared Pictures/PopUp16 and Movies/PopUp16 folders (fallback: files/captures).
static void showToast(const std::string &t);
static std::string captureName() {
    std::string n = stem();
    for (auto &c : n) if (c == '/' || c == ':') c = '-';
    char ts[32]; time_t t = time(nullptr); strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
    return n + "_" + ts;
}
static void takeScreenshot() {
    if (!gameLoaded || !frame.valid) return;
    std::string base = picturesDir + "/" + captureName();
    // flat: every SNES pixel as a sharp 4x4 block
    const int S = 4, W = frame.w * S, H = frame.h * S;
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t c = frame.rgb565[(y / S) * frame.w + x / S];
            uint8_t *p = &rgb[((size_t)y * W + x) * 3];
            p[0] = ((c >> 11) & 31) * 255 / 31; p[1] = ((c >> 5) & 63) * 255 / 63; p[2] = (c & 31) * 255 / 31;
        }
    bool ok = png::writeRGB(base + ".png", W, H, rgb.data());
    // 3D: left and right eye side by side, for 3D photo viewers and VR galleries
    static uint32_t lut[65536];
    static bool lutReady = false;
    if (!lutReady) { stereo::makeLut(lut, true); lutReady = true; }
    stereo::Params sp;
    sp.strength = on(cfg.stereoOn) ? cfg.strength : 0.0f;
    sp.convergence = cfg.convergence; sp.mode7Ramp = on(cfg.mode7Ramp);
    std::vector<uint32_t> sbs;
    unsigned oh = stereo::build(frame, sp, lut, sbs);
    int sw = 2 * stereo::EYE_W;
    std::vector<uint8_t> s3((size_t)sw * oh * 3);
    for (size_t i = 0; i < (size_t)sw * oh; i++) { uint32_t c = sbs[i]; s3[i * 3] = c & 255; s3[i * 3 + 1] = (c >> 8) & 255; s3[i * 3 + 2] = (c >> 16) & 255; }
    ok = png::writeRGB(base + "_3D-SBS.png", sw, (int)oh, s3.data()) && ok;
    trace("screenshot %s %s", base.c_str(), ok ? "saved" : "FAILED");
    showToast(ok ? "Screenshot saved (flat + 3D)" : "Could not save screenshot");
}
static void saveClip() {
    if (!gameLoaded) return;
    if (recorder.busy()) { showToast("Still saving the last clip..."); return; }
    if (recorder.save(moviesDir + "/" + captureName() + ".mp4")) showToast("Saving the last 30 seconds...");
}
