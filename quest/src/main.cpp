// PopUp16 for Meta Quest: standalone OpenXR app running the patched snes9x core.
// Each frame is warped once per eye (shared/stereo.h) and shown on a virtual screen built from two
// quad layers, one visible only to the left eye and one only to the right, so SNES layers sit at
// different depths. Touch controllers or a Bluetooth gamepad drive the game; Left Y opens the menu.

#include <android/log.h>
#include <android/input.h>
#include <android/asset_manager.h>
#include <android/imagedecoder.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "libretro.h"
#include "../../shared/stereo.h"
#include "font8x16.h"
#include "renderer.h"
#include "../../shared/rewind.h"
#include "../../shared/png.h"
#include "../../shared/place_geometry.h"
#include "capture.h"

// Developer test hooks (files/autostart.txt, files/debug_headless, the *_request files) are compiled
// out unless a build explicitly opts in with -DPOPUP16_DEV_HOOKS=1, so a shipped APK cannot be
// driven by files someone drops into its data folder.
#ifdef POPUP16_DEV_HOOKS
static constexpr bool kDevHooks = true;
#else
static constexpr bool kDevHooks = false;
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "PopUp16", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "PopUp16", __VA_ARGS__)
#define XRCHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOGE("%s failed: %d (line %d)", #x, (int)r_, __LINE__); } } while (0)

extern "C" const uint8_t *snes3d_get_layers(void);
extern "C" const uint8_t *snes3d_get_depths(void);
extern "C" void snes3d_enable_planes(int on);
extern "C" const uint16_t *snes3d_get_plane_color(int n);
extern "C" const uint8_t *snes3d_get_plane_z(int n);
extern "C" void snes3d_get_mode7(const int16_t **lines, const uint8_t **vram, const uint16_t **cgram, int *flags);

// input/diagnostic trace kept on the headset (files/input.log) so a normal play session can be read back later
static FILE *traceFile = nullptr;
static void trace(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "PopUp16", "%s", buf);
    if (traceFile) {
        time_t t = time(nullptr); char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
        fprintf(traceFile, "%s %s\n", ts, buf); fflush(traceFile);
    }
}

static double nowSec() {
    timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}


#include "app_settings.h"
#include "app_audio.h"
#include "app_input.h"
#include "app_core.h"
#include "app_capture.h"
#include "app_controls.h"
#include "app_library.h"
#include "app_menu.h"
#include "app_xr.h"
#include "app_menu_input.h"
#include "app_placement.h"
#include "app_pointer.h"
// ---------------------------------------------------------------- main
void android_main(android_app *app) {
    app->onInputEvent = onInput;
    JNIEnv *env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    filesDir = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
    romDir = filesDir + "/roms"; saveDir = filesDir + "/saves"; sysDir = filesDir + "/system";
    mkdir(filesDir.c_str(), 0775); mkdir(romDir.c_str(), 0775); mkdir(saveDir.c_str(), 0775); mkdir(sysDir.c_str(), 0775);
    loadSettings(filesDir + "/settings.cfg", true);
    traceFile = fopen((filesDir + "/input.log").c_str(), "a");
    trace("---- start, ROM folder %s", romDir.c_str());
    {
        aboutLines = {"PopUp16 0.1 - layered 3D for 16-bit console games on Quest",
                      "Made by TyDroElite / CreateShinns LLC. Free, non-commercial software.",
                      "Emulation by the Snes9x team. No games included.",
                      "Support development (optional, unlocks nothing): ko-fi.com/createshinns", ""};
        if (AAsset *as = AAssetManager_open(app->activity->assetManager, "NOTICES.txt", AASSET_MODE_BUFFER)) {
            std::string text((const char *)AAsset_getBuffer(as), AAsset_getLength(as));
            AAsset_close(as);
            size_t pos = 0;
            const size_t width = COLS - 2;
            while (pos <= text.size()) {
                size_t nl = text.find('\n', pos);
                std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                for (auto &c : line) if (c == '\t') c = ' ';
                do { aboutLines.push_back(line.substr(0, width)); line = line.size() > width ? line.substr(width) : ""; } while (!line.empty());
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        } else {
            aboutLines.push_back("(license file missing from this build)");
        }
        trace("about page: %d lines", (int)aboutLines.size());
    }
    // shared media folders the Quest's own gallery and file browser show; app files as a fallback
    picturesDir = "/sdcard/Pictures/PopUp16"; moviesDir = "/sdcard/Movies/PopUp16";
    mkdir("/sdcard/Pictures", 0775); mkdir("/sdcard/Movies", 0775);
    if (mkdir(picturesDir.c_str(), 0775) != 0 && access(picturesDir.c_str(), W_OK) != 0) picturesDir = filesDir + "/captures";
    if (mkdir(moviesDir.c_str(), 0775) != 0 && access(moviesDir.c_str(), W_OK) != 0) moviesDir = filesDir + "/captures";
    mkdir((filesDir + "/captures").c_str(), 0775);
    trace("captures: %s, %s", picturesDir.c_str(), moviesDir.c_str());
    coverDir = filesDir + "/covers";
    mkdir(coverDir.c_str(), 0775);
    loadStats();
    loadMapping(filesDir + "/controls.cfg");
    scanRoms();
    for (auto &[n, g] : stats) if (g.lastPlayed) libTab = TAB_RECENT;
    buildLibView();

    if (!initXR(app)) { LOGE("OpenXR init failed"); ANativeActivity_finish(app->activity); }
    startAudio();
    worker.start();
    {   // test hook: files/autostart.txt names a ROM to load immediately (file is consumed)
        std::vector<uint8_t> a;
        if (kDevHooks && readFile(filesDir + "/autostart.txt", a)) {
            std::string n(a.begin(), a.end());
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            remove((filesDir + "/autostart.txt").c_str());
            if (loadGame(n)) menuMode = MENU_NONE;
        } else if (readFile(filesDir + "/last_game.txt", a)) {
            // quick resume: reopen the last game exactly where it was left
            std::string n(a.begin(), a.end());
            std::vector<uint8_t> st;
            if (n.find('/') == std::string::npos && loadGame(n)) {
                bool restored = readFile(saveDir + "/" + stem() + ".resume", st) && !st.empty() && retro_unserialize(st.data(), st.size());
                menuMode = MENU_NONE;
                if (restored) trace("resumed %s", n.c_str());
                else { trace("resume state for %s could not be restored; starting from its last save", n.c_str());
                       showToast("Couldn't restore where you left off - started from your last save"); }
            }
        }
    }
    int statRuns = 0, statFrames = 0;
    double statT = nowSec();

    bool prevMenuBtn = false;
    double lastEmu = nowSec();
    while (!app->destroyRequested) {
        int events; android_poll_source *source;
        while (ALooper_pollOnce(sessionRunning || !instance || gameLoaded ? 0 : 100, nullptr, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested || !instance) break;
        handleXrEvents(app);
        if (!sessionRunning) {
            // debug: files/debug_headless keeps the game running with the headset off (for adb tests)
            static bool headless = kDevHooks && access((filesDir + "/debug_headless").c_str(), F_OK) == 0;
            if (headless && gameLoaded && menuMode == MENU_NONE) {
                bool in[B_COUNT], rw, ff;
                gameButtons(in, rw, ff);
                memcpy(joypad, in, sizeof joypad);
                double t = nowSec();
                int hr = 0;
                double t0 = nowSec();
                while (t - lastEmu >= 1.0 / avFps) { runFrame(); hr++; lastEmu += 1.0 / avFps; if (t - lastEmu > 0.5) lastEmu = t; }
                if (hr && frame.valid) {  // debug: time the diorama build the headset does per displayed frame
                    static double bMs = 0, part[5]; static int bN = 0;
                    double b0 = nowSec();
                    diorama::Input din{frame.w, frame.h, frame.rgb565.data(), frame.layers.data(), frame.depths.data(), {}, {}};
                    for (int n = 0; n < 5; n++) { din.planeColor[n] = planeColor[n].data(); din.planeZ[n] = planeZ[n].data(); }
                    din.m7lines = m7lines.data(); din.vram = m7vram.data(); din.cgram = m7cgram.data(); din.m7flags = m7flags;
                    sheets.look.on = on(cfg.popLook);
                    sheets.build(din);
                    bMs += (nowSec() - b0) * 1000.0; bN++;
                    for (int k = 0; k < 5; k++) part[k] += sheets.profMs[k];
                    if (bN == 600) { int q7 = 0; for (auto &q : sheets.quads) q7 += q.m7;
                                     trace("headless: diorama build %.2f ms (slices %.2f spans %.2f map %.2f look %.2f sort %.2f), %zu quads (%d floor)",
                                           bMs / bN, part[0] / bN, part[1] / bN, part[2] / bN, part[3] / bN, part[4] / bN, sheets.quads.size(), q7);
                                     bMs = 0; bN = 0; for (double &p2 : part) p2 = 0; }
                }
                static double frameMs = 0; static int frameN = 0;
                if (hr) { frameMs += (nowSec() - t0) * 1000.0 / hr; frameN++; }
                if (frameN == 600) { trace("headless: %.2f ms per frame incl. rewind snapshots, history %zu steps / %zu KB", frameMs / frameN, rewinder.depth(), rewinder.bytes() / 1024); frameMs = 0; frameN = 0; }
                if (access((filesDir + "/menu_request").c_str(), F_OK) == 0) {  // debug: save the library panel as menu.png
                    std::vector<uint8_t> req;
                    readFile(filesDir + "/menu_request", req);
                    remove((filesDir + "/menu_request").c_str());
                    MenuMode keep = menuMode;
                    MenuMode show = MENU_ROMS;
                    if (!req.empty() && req[0] >= '0' && req[0] <= '2') libTab = req[0] - '0';
                    if (!req.empty() && req[0] == 'c') show = MENU_CONTROLS;
                    if (!req.empty() && req[0] == 'h') show = MENU_HELP;
                    if (!req.empty() && (req[0] == 'p' || req[0] == 'q' || req[0] == 'r')) { show = MENU_PAUSE; pausePage = req[0] - 'p'; }
                    if (req.size() > 2) { romSel = atoi((const char *)req.data() + 2); controlsSel = romSel; pauseSel = romSel; }
                    menuMode = show; buildLibView(); renderMenu();
                    std::vector<uint8_t> rgb(MENU_W * MENU_H * 3);
                    for (int i = 0; i < MENU_W * MENU_H; i++) { uint32_t c = menuPixels[i]; rgb[i * 3] = c & 255; rgb[i * 3 + 1] = (c >> 8) & 255; rgb[i * 3 + 2] = (c >> 16) & 255; }
                    png::writeRGB(filesDir + "/menu.png", MENU_W, MENU_H, rgb.data());
                    menuMode = keep;
                    trace("menu dumped");
                }
                {
                    std::string msg;
                    if (recorder.takeResult(msg)) trace("clip: %s", msg.c_str());
                }
                if (access((filesDir + "/shot_request").c_str(), F_OK) == 0) { remove((filesDir + "/shot_request").c_str()); takeScreenshot(); }
                if (access((filesDir + "/clip_request").c_str(), F_OK) == 0) { remove((filesDir + "/clip_request").c_str()); saveClip(); }
                if (access((filesDir + "/rewind_request").c_str(), F_OK) == 0) {  // debug: step back 300 snapshots (15 s)
                    remove((filesDir + "/rewind_request").c_str());
                    int n = 0; double r0 = nowSec();
                    while (n < 300 && rewindStep()) n++;
                    trace("headless: rewound %d steps in %.1f ms", n, (nowSec() - r0) * 1000.0);
                }
                periodicResumeSave();
                usleep(2000);
                if (access((filesDir + "/dump_request").c_str(), F_OK) == 0 && frame.valid) {
                    remove((filesDir + "/dump_request").c_str());
                    std::vector<uint8_t> ppm;
                    char hdr[64]; int n = snprintf(hdr, sizeof hdr, "P6 %u %u 255\n", frame.w, frame.h);
                    ppm.insert(ppm.end(), hdr, hdr + n);
                    for (uint16_t c : frame.rgb565) { ppm.push_back(((c >> 11) & 31) * 255 / 31); ppm.push_back(((c >> 5) & 63) * 255 / 63); ppm.push_back((c & 31) * 255 / 31); }
                    writeFile(filesDir + "/frame.ppm", ppm.data(), ppm.size());
                    LOGI("frame dumped");
                }
            }
            continue;
        }

        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) { usleep(2000); continue; }
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(session, &fbi))) continue;  // never end a frame we could not begin

        pollActions();
        {   // head pose for menus and placement presets
            XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(viewSpace, localSpace, fs.predictedDisplayTime, &hl)) &&
                (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                headX = hl.pose.position.x; headY = hl.pose.position.y; headZ = hl.pose.position.z;
                Quat hq{hl.pose.orientation.x, hl.pose.orientation.y, hl.pose.orientation.z, hl.pose.orientation.w};
                XrVector3f f = qrot(hq, v3(0, 0, -1));
                headYaw = atan2f(-f.x, -f.z);
            }
            static bool placedOnce = false;
            if (!placedOnce || recenterPending) { placeMenu(); placedOnce = true; recenterPending = false; }
        }
        renderer.shapes.clear();
        pointerUpdate(fs.predictedDisplayTime);
        bool all[B_COUNT], nav[B_COUNT], wantRewind, wantFast;
        gameButtons(all, wantRewind, wantFast);
        menuButtons(nav);
        {
            static const char *names[B_COUNT] = {"B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R"};
            static bool last[B_COUNT];
            static bool lastMenu;
            for (int i = 0; i < B_COUNT; i++)
                if (all[i] != last[i]) { trace("input SNES %s %s (menu mode %d, game %d)", names[i], all[i] ? "down" : "up", (int)menuMode, (int)gameLoaded); last[i] = all[i]; }
            if (xrMenu != lastMenu) { trace("input PopUp16-menu button %s", xrMenu ? "down" : "up"); lastMenu = xrMenu; }
        }
        {   // debug (with files/debug_headless): capture requests from adb, also while the headset is worn
            static bool dbg = kDevHooks && access((filesDir + "/debug_headless").c_str(), F_OK) == 0;
            static double nextCheck = 0;
            if (dbg && nowSec() > nextCheck) {
                nextCheck = nowSec() + 0.5;
                if (access((filesDir + "/shot_request").c_str(), F_OK) == 0) { remove((filesDir + "/shot_request").c_str()); takeScreenshot(); }
                if (access((filesDir + "/clip_request").c_str(), F_OK) == 0) { remove((filesDir + "/clip_request").c_str()); saveClip(); }
            }
        }
        bool menuBtn = xrMenu || padMenu || (physDown[PH_PAD_SELECT] && physDown[PH_PAD_START]);
        bool menuEdge = menuBtn && !prevMenuBtn;
        if (menuEdge && remapAction >= 0) { remapAction = -1; menuDirty = true; menuEdge = false; }  // cancels a remap
        if (menuEdge) {
            if (menuMode == MENU_NONE) { menuMode = MENU_PAUSE; pausePage = 0; pauseSel = 0; saveSram(); placeMenu(); }
            else if (gameLoaded) { menuMode = MENU_NONE; saveGlobal(); saveSettings(saveDir + "/" + stem() + ".cfg", false); }
            menuDirty = true;
            resetMenuInput();  // swallow whatever is held when the menu opens
        }
        prevMenuBtn = menuBtn;

        if (menuMode != MENU_NONE) {
            memset(joypad, 0, sizeof joypad);
            MenuMode before = menuMode;
            if (remapAction >= 0 || remapJustDone) { int was = remapAction; remapCapture(); resetMenuInput(); if (was != remapAction) menuDirty = true; }
            else menuInput(nav);
            if (before != MENU_NONE && menuMode == MENU_NONE) {
                saveGlobal();
                if (gameLoaded) saveSettings(saveDir + "/" + stem() + ".cfg", false);
                for (int i = 0; i < B_COUNT; i++) joypad[i] = false;
            }
            lastEmu = nowSec();
        } else if (gameLoaded) {
            memcpy(joypad, all, sizeof joypad);
            // pace by audio: keep ~3 video frames of sound queued
            uint32_t target = (uint32_t)(OUT_RATE / avFps * 3);
            int runs = 0;
            {
                static bool prevShot = false, prevClip = false;
                if (wantShot && !prevShot) takeScreenshot();
                if (wantClip && !prevClip) saveClip();
                prevShot = wantShot; prevClip = wantClip;
            }
            bool rewinding = wantRewind;
            // game speed: the emulator follows the audio clock, so stretching the audio slows the game
            float speed = std::clamp(cfg.speed * (wantFast ? 2.0f : 1.0f), 0.25f, 4.0f);
            resampleStep = avRate * speed / OUT_RATE;
            if (rewinding) {
                double t = nowSec();
                if (t - lastEmu > 0.25) lastEmu = t;
                while (t - lastEmu >= 1.0 / avFps && runs < 2) { rewindStep(); runs++; lastEmu += 1.0 / avFps; }
            } else if (aaStream) { while (ringFill() < target && runs < 4) { runFrame(); runs++; } lastEmu = nowSec(); }
            else { double t = nowSec(); while (t - lastEmu >= 1.0 / (avFps * speed) && runs < 4) { runFrame(); runs++; lastEmu += 1.0 / (avFps * speed); } }
            statRuns += runs;
            {   // playtime, and a cover captured from play for games that have none
                static double lastT = nowSec();
                double t = nowSec(), dt = std::min(t - lastT, 0.1);
                lastT = t;
                if (runs) {
                    stats[gameName].seconds += dt;
                    playedThisLoad += dt;
                    if (playedThisLoad > 30 && playedThisLoad - dt <= 30 && frame.valid) {
                        std::string out = coverDir + "/" + stem() + ".png";
                        struct stat sb;
                        bool any = false;
                        for (const char *e : {".png", ".jpg", ".jpeg", ".webp"}) any |= stat((coverDir + "/" + stem() + e).c_str(), &sb) == 0;
                        if (!any && png::write565(out, (int)frame.w, (int)frame.h, frame.rgb565.data())) { coverCache.erase(stem()); trace("captured cover for %s", stem().c_str()); }
                    }
                }
            }
            periodicResumeSave();
        }

        // eye poses in the room (LOCAL space); their separation drives the comfort clamp
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        bool haveViews = false;
        float ipd = 0.063f;
        {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime; vli.space = localSpace;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t vn = 0;
            if (XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &vn, views)) && vn == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                haveViews = true;
                float dx = views[1].pose.position.x - views[0].pose.position.x;
                float dy = views[1].pose.position.y - views[0].pose.position.y;
                float dz = views[1].pose.position.z - views[0].pose.position.z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d > 0.045f && d < 0.08f) ipd = d;
            }
        }

        if (newFrame && frame.valid) {  // hand the frame to the build thread (replacing any older pending one)
            {
                std::lock_guard<std::mutex> l(worker.m);
                BuildWorker::Job &j = worker.job;
                j.w = frame.w; j.h = frame.h;
                j.rgb565 = frame.rgb565; j.layers = frame.layers; j.depths = frame.depths;
                for (int n = 0; n < 5; n++) { j.planeColor[n] = planeColor[n]; j.planeZ[n] = planeZ[n]; }
                j.lines = m7lines; j.vram = m7vram; j.cgram = m7cgram; j.flags = m7flags;
                j.ramp = on(cfg.mode7Ramp); j.look = on(cfg.popLook); j.backdrop = on(cfg.sky) || !on(cfg.room);
                j.gen = gameGen;
                worker.hasJob = true;
            }
            worker.cv.notify_one();
            newFrame = false;
        }
        {   // upload the newest finished build
            int r;
            { std::lock_guard<std::mutex> l(worker.m); r = worker.ready; worker.ready = -1; worker.uploading = r; }
            if (r >= 0 && worker.builtGen[r] != gameGen) {  // built from the previous game: drop it
                std::lock_guard<std::mutex> l(worker.m);
                worker.uploading = -1;
                r = -1;
            }
            if (r >= 0) {
                diorama::Builder &b = worker.builders[r];
                renderer.haze = b.look.on ? b.look.haze : 0.0f;
                renderer.upload(b, b.lastW, b.lastH);
                std::lock_guard<std::mutex> l(worker.m);
                worker.uploading = -1;
            }
        }
        {
            std::string msg;
            if (recorder.takeResult(msg)) { showToast(msg); trace("clip: %s", msg.c_str()); }
        }
        if (menuMode == MENU_NONE && !toast.empty() && nowSec() > toastUntil) toast.clear();
        if ((menuMode != MENU_NONE || !toast.empty()) && (menuDirty || (!toast.empty() && nowSec() > toastUntil))) {
            if (nowSec() > toastUntil) toast.clear();
            renderMenu();
            uploadSwap(menuSwap, menuPixels.data(), MENU_W, MENU_H);
        }

        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        if (menuMode == MENU_ARRANGE) arrangeUpdate(fs.predictedDisplayTime);
        setPassthrough(on(cfg.room) && hasPassthrough);
        XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerQuad quads[2];
        const XrCompositionLayerBaseHeader *layers[4];
        uint32_t nl = 0;
        bool roomVisible = passthroughRunning && passthroughLayer != XR_NULL_HANDLE && fs.shouldRender;
        if (roomVisible) {
            ptLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            ptLayer.layerHandle = passthroughLayer;
            layers[nl++] = (XrCompositionLayerBaseHeader *)&ptLayer;
        }
        if (fs.shouldRender && haveViews) {
            // One SNES pixel spans screenWidth/256 metres. Scale so the deepest sheet (~8.5 px) lands at
            // 80% of the eye separation; the shader never lets a shift reach 90% of it (no divergence).
            float pxM = cfg.screenWidth / 256.0f;
            float autoScale = (0.80f * ipd / pxM) / 8.5f;
            float viewDist = placementDepth();
            float screen[4] = {cfg.screenWidth, cfg.screenWidth * 3.0f / 4.0f, viewDist, pxM};
            float model[16];
            placementMatrix(model);
            // box style: a full-depth stack is about a third of the width deep, whatever the comfort scale
            float style[2] = {(cfg.box > 0.5f && cfg.box < 1.5f) ? 1.0f : 0.0f, 0.04f * cfg.screenWidth / std::max(autoScale, 1e-3f)};
            buildWindow(model);
            float depth[4] = {ipd, on(cfg.stereoOn) ? cfg.strength * autoScale : 0.0f, on(cfg.stereoOn) ? cfg.convergence * autoScale : 0.0f, 0.0f};
            for (int eye = 0; eye < 2; eye++) {
                render::Eye &e = renderer.eyes[eye];
                uint32_t idx;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(e.swap, &ai, &idx);
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(e.swap, &wi);
                // swapped eyes: each eye is drawn from the other eye's position
                const XrView &v = views[on(cfg.swapEyes) ? 1 - eye : eye];
                XrView drawView = views[eye];
                drawView.pose.position = v.pose.position;
                renderer.drawEye(eye, idx, drawView, screen, depth, model, style, roomVisible ? 0.0f : 1.0f, gameLoaded && frame.valid);
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(e.swap, &ri);
                pviews[eye].pose = views[eye].pose;
                pviews[eye].fov = views[eye].fov;
                pviews[eye].subImage.swapchain = e.swap;
                pviews[eye].subImage.imageRect = {{0, 0}, {e.w, e.h}};
            }
            proj.space = localSpace;
            if (roomVisible) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            proj.viewCount = 2;
            proj.views = pviews;
            layers[nl++] = (XrCompositionLayerBaseHeader *)&proj;
        }
        bool notice = menuMode == MENU_NONE && !toast.empty() && nowSec() < toastUntil;
        if (fs.shouldRender && (menuMode != MENU_NONE || notice)) {
            XrCompositionLayerQuad &q = quads[0];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.space = localSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = menuSwap.handle;
            q.subImage.imageRect = {{0, 0}, {MENU_W, MENU_H}};
            q.pose.orientation.w = 1;
            if (notice) {  // in-game notice: the top strip of the menu image, small and low in view
                const int stripH = 3 * CELL_H;
                q.subImage.imageRect = {{0, MENU_H - stripH}, {MENU_W, stripH}};  // GL rows count from the bottom
                q.pose.position = {0.0f, -0.45f, -1.1f};
                q.size = {0.6f, 0.6f * stripH / MENU_W};
            } else if (menuMode == MENU_ARRANGE) {  // small hint card low in view, out of the way of the game
                q.pose.position = {0.0f, -0.55f, -1.0f};
                q.size = {0.6f, 0.6f * MENU_H / MENU_W};
            } else {  // the menu panel, placed in front of you when it opened, as tall as its page
                XrVector3f pc; float ph;
                panelFrame(pc, ph);
                q.subImage.imageRect = {{0, MENU_H - panelH}, {MENU_W, panelH}};  // GL rows count from the bottom
                q.pose.position = pc;
                q.pose.orientation = {menuRot.x, menuRot.y, menuRot.z, menuRot.w};
                q.size = {MENU_WM, ph};
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;  // rounded corners
            }
            layers[nl] = (XrCompositionLayerBaseHeader *)&q;
            nl++;
            if (!notice && menuMode != MENU_ARRANGE && ptr.hover && cursorSwap.handle) {  // the pointer dot on the panel
                XrVector3f pc; float ph;
                panelFrame(pc, ph);
                XrCompositionLayerQuad &c = quads[1];
                c = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                c.space = localSpace;
                c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                c.subImage.swapchain = cursorSwap.handle;
                c.subImage.imageRect = {{0, 0}, {32, 32}};
                c.pose.position = vadd(pc, qrot(menuRot, v3(ptr.lx, ptr.ly, 0.003f)));
                c.pose.orientation = {menuRot.x, menuRot.y, menuRot.z, menuRot.w};
                c.size = {0.016f, 0.016f};
                c.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                layers[nl++] = (XrCompositionLayerBaseHeader *)&c;
            }
        }
        statFrames++;
        if (nowSec() - statT >= 5.0) {
            LOGI("emu %.1f fps, display %.1f fps, audio queue %u, layers %u", statRuns / (nowSec() - statT),
                 statFrames / (nowSec() - statT), ringFill(), nl);
            statT = nowSec(); statRuns = statFrames = 0;
        }
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = nl; fei.layers = layers;
        XrResult er = xrEndFrame(session, &fei);
        static XrResult lastEnd = XR_SUCCESS;  // report a rejected frame once, not 90 times a second
        if (er != lastEnd) { trace("xrEndFrame -> %d (%u layers)", (int)er, nl); lastEnd = er; }
    }

    worker.stop();
    unloadGame();
    saveGlobal();
    stopAudio();
    if (session) { destroyPassthrough(); xrDestroySession(session); }
    if (instance) xrDestroyInstance(instance);
    app->activity->vm->DetachCurrentThread();
}
