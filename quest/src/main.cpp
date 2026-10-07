// PopUp16 for Meta Quest: standalone OpenXR app running the patched snes9x core.
// Each frame is warped once per eye (shared/stereo.h) and shown on a virtual screen built from two
// quad layers, one visible only to the left eye and one only to the right, so SNES layers sit at
// different depths. Touch controllers or a Bluetooth gamepad drive the game; Left Y opens the menu.

#include <android/log.h>
#include <android/input.h>
#include <android/asset_manager.h>
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

#include "libretro.h"
#include "../../shared/stereo.h"
#include "font8x16.h"
#include "renderer.h"
#include "../../shared/rewind.h"

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

// ---------------------------------------------------------------- settings
struct Settings {
    // per game
    float strength = 1.0f, convergence = 0.0f, mode7Ramp = 1, swapEyes = 0, stereoOn = 1;
    // global: how and where the diorama sits in the room
    float screenWidth = 2.4f, distance = 2.2f, room = 0, box = 0, popLook = 1, sky = 1, speed = 1;
    float px = 0, py = 0, pz = -2.2f, qx = 0, qy = 0, qz = 0, qw = 1;
};
static Settings cfg;
static std::string filesDir, romDir, saveDir, sysDir;
static const struct { const char *name; float Settings::*field; bool global; } kFields[] = {
    {"strength", &Settings::strength, false}, {"convergence", &Settings::convergence, false},
    {"mode7Ramp", &Settings::mode7Ramp, false}, {"swapEyes", &Settings::swapEyes, false},
    {"stereoOn", &Settings::stereoOn, false}, {"screenWidth", &Settings::screenWidth, true},
    {"distance", &Settings::distance, true}, {"room", &Settings::room, true}, {"box", &Settings::box, true},
    {"popLook", &Settings::popLook, true}, {"sky", &Settings::sky, true}, {"speed", &Settings::speed, true},
    {"px", &Settings::px, true}, {"py", &Settings::py, true}, {"pz", &Settings::pz, true},
    {"qx", &Settings::qx, true}, {"qy", &Settings::qy, true}, {"qz", &Settings::qz, true}, {"qw", &Settings::qw, true},
};
static bool on(float v) { return v > 0.5f; }
static void flip(float &v) { v = on(v) ? 0.0f : 1.0f; }

static void saveSettings(const std::string &path, bool global) {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    for (auto &k : kFields) if (k.global == global) fprintf(f, "%s=%g\n", k.name, cfg.*k.field);
    fclose(f);
}
static void loadSettings(const std::string &path, bool global) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char k[64]; float v;
    while (fscanf(f, "%63[^=]=%f\n", k, &v) == 2)
        for (auto &e : kFields) if (e.global == global && !strcmp(e.name, k)) cfg.*e.field = v;
    fclose(f);
}
static void saveGlobal() { saveSettings(filesDir + "/settings.cfg", true); }

static bool readFile(const std::string &p, std::vector<uint8_t> &out) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}
static void writeFile(const std::string &p, const void *d, size_t n) {
    FILE *f = fopen(p.c_str(), "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

// ---------------------------------------------------------------- audio
// Single-producer (emulator thread) / single-consumer (AAudio callback) ring of stereo int16 frames.
static const int OUT_RATE = 48000;
static const uint32_t RING = 1 << 15;
static int16_t ring[RING * 2];
static std::atomic<uint32_t> ringW{0}, ringR{0};
static double resampleStep = 32040.5 / OUT_RATE, resamplePos = 0.0;
static int16_t prevL = 0, prevR = 0;
static AAudioStream *aaStream = nullptr;
static bool muteAudio = false;  // rewind plays back silently

static uint32_t ringFill() { return ringW.load(std::memory_order_acquire) - ringR.load(std::memory_order_acquire); }
static void pushOut(int16_t l, int16_t r) {
    uint32_t w = ringW.load(std::memory_order_relaxed);
    if (w - ringR.load(std::memory_order_acquire) >= RING) return;  // full: drop
    ring[(w & (RING - 1)) * 2] = l; ring[(w & (RING - 1)) * 2 + 1] = r;
    ringW.store(w + 1, std::memory_order_release);
}
static void pushIn(int16_t l, int16_t r) {  // linear resample core rate -> 48 kHz
    while (resamplePos <= 1.0) {
        pushOut((int16_t)(prevL + (l - prevL) * resamplePos), (int16_t)(prevR + (r - prevR) * resamplePos));
        resamplePos += resampleStep;
    }
    resamplePos -= 1.0;
    prevL = l; prevR = r;
}
static aaudio_data_callback_result_t aaCallback(AAudioStream *, void *, void *audioData, int32_t numFrames) {
    int16_t *out = (int16_t *)audioData;
    uint32_t r = ringR.load(std::memory_order_relaxed), w = ringW.load(std::memory_order_acquire);
    for (int i = 0; i < numFrames; i++) {
        if (r != w) { out[i * 2] = ring[(r & (RING - 1)) * 2]; out[i * 2 + 1] = ring[(r & (RING - 1)) * 2 + 1]; r++; }
        else { out[i * 2] = out[i * 2 + 1] = 0; }
    }
    ringR.store(r, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void startAudio() {
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, OUT_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aaCallback, nullptr);
    if (AAudioStreamBuilder_openStream(b, &aaStream) != AAUDIO_OK) aaStream = nullptr;
    AAudioStreamBuilder_delete(b);
    if (aaStream) AAudioStream_requestStart(aaStream);
    LOGI("audio %s", aaStream ? "started" : "unavailable");
}
static void stopAudio() {
    if (aaStream) { AAudioStream_requestStop(aaStream); AAudioStream_close(aaStream); aaStream = nullptr; }
}

// ---------------------------------------------------------------- input
enum { B_B, B_Y, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_X, B_L, B_R, B_COUNT };  // RETRO_DEVICE_ID_JOYPAD order
static bool xrButtons[B_COUNT], padButtons[B_COUNT];
static bool padMenu = false, xrMenu = false;
static bool padRewind = false, padFast = false, xrRewind = false;
static float navX = 0, navY = 0, trigRValue = 0;  // menu: right stick X, left stick Y, right trigger
static float padAxisX = 0, padAxisY = 0;
static bool joypad[B_COUNT];

static int keyToButton(int32_t key) {
    switch (key) {
    case AKEYCODE_BUTTON_A: return B_B;
    case AKEYCODE_BUTTON_B: return B_A;
    case AKEYCODE_BUTTON_X: return B_Y;
    case AKEYCODE_BUTTON_Y: return B_X;
    case AKEYCODE_BUTTON_L1: return B_L;
    case AKEYCODE_BUTTON_R1: return B_R;
    case AKEYCODE_BUTTON_START: return B_START;
    case AKEYCODE_BUTTON_SELECT: return B_SELECT;
    case AKEYCODE_DPAD_UP: return B_UP;
    case AKEYCODE_DPAD_DOWN: return B_DOWN;
    case AKEYCODE_DPAD_LEFT: return B_LEFT;
    case AKEYCODE_DPAD_RIGHT: return B_RIGHT;
    default: return -1;
    }
}
static int32_t onInput(android_app *, AInputEvent *e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e);
        bool down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        if (key == AKEYCODE_BUTTON_MODE || key == AKEYCODE_BUTTON_THUMBL) { padMenu = down; return 1; }
        if (key == AKEYCODE_BUTTON_L2) { padRewind = down; return 1; }   // hold to rewind
        if (key == AKEYCODE_BUTTON_R2) { padFast = down; return 1; }     // hold to fast-forward
        int b = keyToButton(key);
        trace("gamepad key %d %s", key, down ? "down" : "up");
        if (b >= 0) { padButtons[b] = down; return 1; }
    } else if (type == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(e) & AINPUT_SOURCE_JOYSTICK)) {
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        padAxisX = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        padAxisY = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        if (fabsf(hx) > 0.5f || fabsf(hy) > 0.5f || (fabsf(padAxisX) < 0.5f && fabsf(padAxisY) < 0.5f)) {
            padAxisX = fabsf(hx) > fabsf(padAxisX) ? hx : padAxisX;
            padAxisY = fabsf(hy) > fabsf(padAxisY) ? hy : padAxisY;
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- core
static stereo::Frame frame;
static std::vector<uint16_t> planeColor[5];
static std::vector<uint8_t> planeZ[5];
static diorama::Builder sheets;  // used by the headless debug timing only

// Sheet building runs on its own thread: the display loop hands over a copy of each new frame and
// uploads whichever build finished last, so a slow build never delays a headset frame.
struct BuildWorker {
    struct Job {
        unsigned w = 0, h = 0;
        std::vector<uint16_t> rgb565, planeColor[5];
        std::vector<uint8_t> layers, depths, planeZ[5], vram;
        std::vector<int16_t> lines;
        std::vector<uint16_t> cgram;
        int flags = 0;
        bool ramp = true, look = true, backdrop = true;
    };
    Job job;                      // filled by the display loop under lock
    bool hasJob = false, quit = false;
    diorama::Builder builders[3];     // three, so building never touches the one waiting or uploading
    int ready = -1, uploading = -1;   // finished build waiting for upload; build being uploaded
    std::mutex m;
    std::condition_variable cv;
    std::thread th;

    void start() { th = std::thread([this] { loop(); }); }
    void stop() { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_one(); if (th.joinable()) th.join(); }
    void loop() {
        Job local;
        while (true) {
            int target;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [this] { return hasJob || quit; });
                if (quit) return;
                std::swap(local, job);
                hasJob = false;
                target = 0;
                while (target == ready || target == uploading) target++;
            }
            diorama::Builder &b = builders[target];
            diorama::Input din{local.w, local.h, local.rgb565.data(), local.layers.data(), local.depths.data(), {}, {}};
            for (int n = 0; n < 5; n++) { din.planeColor[n] = local.planeColor[n].data(); din.planeZ[n] = local.planeZ[n].data(); }
            if (!local.lines.empty()) { din.m7lines = local.lines.data(); din.vram = local.vram.data(); din.cgram = local.cgram.data(); din.m7flags = local.flags; }
            b.mode7Ramp = local.ramp; b.look.on = local.look; b.showBackdrop = local.backdrop;
            b.build(din);
            std::lock_guard<std::mutex> l(m);
            ready = target;
        }
    }
};
static BuildWorker worker;
// Mode 7 state copied with each frame (the core keeps rendering the next one meanwhile)
static std::vector<int16_t> m7lines(240 * 8);
static std::vector<uint8_t> m7vram(0x10000);
static std::vector<uint16_t> m7cgram(256);
static int m7flags = 0;
static render::Renderer renderer;
static bool newFrame = false;
static bool gameLoaded = false;
static std::string gameName, gamePath;
static double avFps = 60.0988, avRate = 32040.5;

static void core_log(enum retro_log_level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); __android_log_vprint(ANDROID_LOG_INFO, "snes9x", fmt, ap); va_end(ap);
}
static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(const retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = sysDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = saveDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    default: return false;
    }
}
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    frame.capture(data, w, h, pitch, snes3d_get_layers(), snes3d_get_depths());
    {
        const int16_t *l; const uint8_t *vr; const uint16_t *cg;
        snes3d_get_mode7(&l, &vr, &cg, &m7flags);
        memcpy(m7lines.data(), l, 240 * 8 * 2);
        memcpy(m7cgram.data(), cg, 512);
        // VRAM is only needed when Mode 7 is on screen; skip the 64 KiB copy otherwise
        bool any7 = false;
        for (size_t i = 0; i < frame.layers.size() && !any7; i += 3) any7 = frame.layers[i] == 6;
        if (any7) memcpy(m7vram.data(), vr, 0x10000);
    }
    size_t ppl = pitch / 2;
    for (int n = 0; n < 5; n++) {
        const uint16_t *c = snes3d_get_plane_color(n);
        const uint8_t *z = snes3d_get_plane_z(n);
        planeColor[n].resize(w * h); planeZ[n].resize(w * h);
        for (unsigned y = 0; y < h; y++) {
            memcpy(&planeColor[n][y * w], c + y * ppl, w * 2);
            memcpy(&planeZ[n][y * w], z + y * ppl, w);
        }
    }
    newFrame = true;
}
static void audio_sample(int16_t l, int16_t r) { if (!muteAudio) pushIn(l, r); }
static size_t audio_batch(const int16_t *d, size_t n) { if (muteAudio) return n; for (size_t i = 0; i < n; i++) pushIn(d[i * 2], d[i * 2 + 1]); return n; }
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= B_COUNT) return 0;
    return joypad[id] ? 1 : 0;
}

static Rewind rewinder;
static int framesSinceSnap = 0;
static void runFrame() {  // one emulated frame, snapshotting every third for rewind
    retro_run();
    if (++framesSinceSnap >= 3) {
        framesSinceSnap = 0;
        static std::vector<uint8_t> st;
        st.resize(retro_serialize_size());
        if (!st.empty() && retro_serialize(st.data(), st.size())) rewinder.push(st);
    }
}
static bool rewindStep() {  // one step back in history, shown by running one silent frame
    static std::vector<uint8_t> st;
    if (!rewinder.pop(st) || !retro_unserialize(st.data(), st.size())) return false;
    muteAudio = true; retro_run(); muteAudio = false;
    return true;
}

static std::string stem() { return gameName.substr(0, gameName.find_last_of('.')); }
static void saveSram() {
    if (!gameLoaded) return;
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (n) writeFile(saveDir + "/" + stem() + ".srm", retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), n);
}
// quick resume: the running game's state and name are saved whenever the app may go away
static void saveResume() {
    if (!gameLoaded) return;
    std::vector<uint8_t> s(retro_serialize_size());
    if (!s.empty() && retro_serialize(s.data(), s.size())) {
        writeFile(saveDir + "/" + stem() + ".resume", s.data(), s.size());
        writeFile(filesDir + "/last_game.txt", gameName.data(), gameName.size());
    }
}
static void periodicResumeSave() {  // crash safety while playing
    static double next = nowSec() + 60;
    if (nowSec() > next) { saveResume(); next = nowSec() + 60; }
}
static void unloadGame() {
    if (!gameLoaded) return;
    saveResume();
    saveSram();
    saveSettings(saveDir + "/" + stem() + ".cfg", false);
    retro_unload_game();
    retro_deinit();
    gameLoaded = false;
    frame.valid = false;
}
static bool loadGame(const std::string &name) {
    unloadGame();
    std::vector<uint8_t> rom;
    std::string path = romDir + "/" + name;
    if (!readFile(path, rom)) { LOGE("cannot read %s", path.c_str()); return false; }
    retro_set_environment(environment);
    retro_init();
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    static std::vector<uint8_t> keep;  // the core may keep pointers into the ROM data
    keep.swap(rom);
    retro_game_info gi = {path.c_str(), keep.data(), keep.size(), nullptr};
    if (!retro_load_game(&gi)) { LOGE("core refused %s", name.c_str()); retro_deinit(); return false; }
    snes3d_enable_planes(1);
    rewinder.clear(); framesSinceSnap = 0;
    gameLoaded = true; gameName = name; gamePath = path;
    retro_system_av_info av; retro_get_system_av_info(&av);
    avFps = av.timing.fps; avRate = av.timing.sample_rate;
    resampleStep = avRate / OUT_RATE;
    std::vector<uint8_t> s;
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (n && readFile(saveDir + "/" + stem() + ".srm", s) && s.size() == n) memcpy(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), s.data(), n);
    loadSettings(saveDir + "/" + stem() + ".cfg", false);
    trace("loaded %s (%.3f fps, %.1f Hz)", name.c_str(), avFps, avRate);
    return true;
}

// ---------------------------------------------------------------- menu
static const int MENU_W = 1024, MENU_H = 768, CELL_W = 16, CELL_H = 32;
static const int COLS = MENU_W / CELL_W, ROWS = MENU_H / CELL_H;
static std::vector<uint32_t> menuPixels(MENU_W * MENU_H);
enum MenuMode { MENU_NONE, MENU_ROMS, MENU_PAUSE, MENU_HELP, MENU_ABOUT, MENU_ARRANGE, MENU_SLOTS };
static int slotSel = 0;
static bool slotSaving = true;
static std::vector<std::string> aboutLines;
static int aboutTop = 0;
static MenuMode menuMode = MENU_ROMS;
static std::vector<std::string> roms;
static int romSel = 0, pauseSel = 0;
static std::string toast;
static double toastUntil = 0;
static bool menuDirty = true;

static void scanRoms() {
    roms.clear();
    if (DIR *d = opendir(romDir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            std::string l = n; for (auto &c : l) c = (char)tolower(c);
            if (l.size() > 4 && (l.ends_with(".sfc") || l.ends_with(".smc"))) roms.push_back(n);
        }
        closedir(d);
    }
    std::sort(roms.begin(), roms.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    romSel = std::clamp(romSel, 0, std::max(0, (int)roms.size() - 1));
}

static void drawText(int col, int row, const std::string &s, uint32_t color, uint32_t bg = 0) {
    for (size_t i = 0; i < s.size() && col + (int)i < COLS; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        const unsigned char *g = kFont8x16[c - 32];
        int x0 = (col + (int)i) * CELL_W, y0 = row * CELL_H;
        for (int y = 0; y < CELL_H; y++)
            for (int x = 0; x < CELL_W; x++) {
                bool on = g[y / 2] & (0x80 >> (x / 2));
                uint32_t &px = menuPixels[(y0 + y) * MENU_W + x0 + x];
                if (on) px = color; else if (bg) px = bg;
            }
    }
}
static const uint32_t C_BG = 0xff201812, C_TEXT = 0xffe0e0e0, C_DIM = 0xff909090, C_HI = 0xff30c0ff, C_SELBG = 0xff604020;

static const char *helpLines[] = {
    "CONTROLS (Touch controllers)",
    "",
    "Move ............ either thumbstick",
    "SNES B (jump) ... right A",
    "SNES A .......... right B",
    "SNES Y (run) .... right trigger",
    "SNES X .......... left trigger",
    "SNES L / R ...... left grip / right grip",
    "Start ........... left menu button or right stick click",
    "Select .......... left X",
    "Rewind .......... hold the left stick in (gamepad: hold L2)",
    "PopUp16 menu ..... left Y",
    "",
    "Bluetooth gamepads work too (menu: Select+Start, fast-forward: R2).",
    "",
    "Press any button to play."};
enum PauseItem { P_RESUME, P_CONTROLS, P_SPEED, P_DEPTH, P_CONV, P_3D, P_MODE7, P_SWAP, P_ROOM, P_STYLE, P_LOOK, P_SKY,
                 P_ARRANGE, P_RESET, P_SIZE, P_DIST, P_SAVE, P_LOAD, P_GAMES, P_ABOUT, PAUSE_N };
static const char *pauseItems[PAUSE_N] = {
    "Resume", "Controls", "Game speed", "3D depth", "Convergence", "3D on/off", "Mode 7 floor depth", "Swap eyes",
    "Surroundings", "3D style", "Pop-up look", "Show sky", "Move & resize...", "Reset position",
    "Screen size", "Screen distance", "Save state", "Load state", "Choose game", "About & licenses"};
static bool adjustable(int i) { return i == P_SPEED || i == P_DEPTH || i == P_CONV || i == P_SIZE || i == P_DIST; }
static bool toggle(int i) { return i == P_3D || i == P_MODE7 || i == P_SWAP || i == P_ROOM || i == P_STYLE || i == P_LOOK || i == P_SKY; }
static void defaultPlacement() {
    cfg.px = 0; cfg.py = 0; cfg.pz = -cfg.distance; cfg.qx = cfg.qy = cfg.qz = 0; cfg.qw = 1;
}

static void showToast(const std::string &t);
// ---- save slots: state + a thumbnail of the frame + the time it was made
static std::string slotPath(int n) { return saveDir + "/" + stem() + (n == 0 ? ".state" : ".slot" + std::to_string(n + 1) + ".state"); }
static void saveSlot(int n) {
    std::vector<uint8_t> st(retro_serialize_size());
    if (st.empty() || !retro_serialize(st.data(), st.size())) { showToast("Could not save"); return; }
    writeFile(slotPath(n), st.data(), st.size());
    if (frame.valid) {
        std::vector<uint8_t> th(4 + frame.rgb565.size() * 2);
        uint16_t wh[2] = {(uint16_t)frame.w, (uint16_t)frame.h};
        memcpy(th.data(), wh, 4); memcpy(th.data() + 4, frame.rgb565.data(), frame.rgb565.size() * 2);
        writeFile(slotPath(n) + ".thumb", th.data(), th.size());
    }
    showToast("Saved to slot " + std::to_string(n + 1));
}
static bool loadSlot(int n) {
    std::vector<uint8_t> st;
    return readFile(slotPath(n), st) && retro_unserialize(st.data(), st.size());
}
static void drawSlots() {
    char buf[96];
    drawText(1, 0, slotSaving ? "Save to which slot?" : "Load which slot?", C_HI);
    for (int n = 0; n < 4; n++) {
        int c = n % 2, r = n / 2, x0 = 16 + c * 504, y0 = 48 + r * 336, labelRow = r == 0 ? 10 : 21;
        if (n == slotSel)  // highlight frame
            for (int y = y0; y < y0 + 312; y++)
                for (int x = x0; x < x0 + 496; x++)
                    if (y < y0 + 4 || y >= y0 + 308 || x < x0 + 4 || x >= x0 + 492) menuPixels[y * MENU_W + x] = C_HI;
        std::vector<uint8_t> th;
        struct stat sb;
        bool exists = stat(slotPath(n).c_str(), &sb) == 0;
        if (exists && readFile(slotPath(n) + ".thumb", th) && th.size() > 4) {
            uint16_t wh[2]; memcpy(wh, th.data(), 4);
            const uint16_t *px = (const uint16_t *)(th.data() + 4);
            if (th.size() >= 4 + (size_t)wh[0] * wh[1] * 2) {
                const int tw = 292, tht = 256, tx = x0 + 102, ty = y0 + 28;
                for (int y = 0; y < tht; y++)
                    for (int x = 0; x < tw; x++) {
                        uint16_t c565 = px[(y * wh[1] / tht) * wh[0] + x * wh[0] / tw];
                        uint32_t rr = ((c565 >> 11) & 31) * 255 / 31, gg = ((c565 >> 5) & 63) * 255 / 63, bb = (c565 & 31) * 255 / 31;
                        menuPixels[(ty + y) * MENU_W + tx + x] = 0xff000000u | (bb << 16) | (gg << 8) | rr;
                    }
            }
        }
        if (exists) {
            char ts[32]; strftime(ts, sizeof ts, "%b %d %H:%M", localtime(&sb.st_mtime));
            snprintf(buf, sizeof buf, "Slot %d  %s", n + 1, ts);
        } else snprintf(buf, sizeof buf, "Slot %d  (empty)", n + 1);
        drawText(x0 / CELL_W + 6, labelRow, buf, n == slotSel ? 0xffffffff : C_TEXT);
    }
    drawText(1, ROWS - 1, "sticks: choose slot   A: confirm   B: back", C_DIM);
}

static void renderMenu() {
    std::fill(menuPixels.begin(), menuPixels.end(), C_BG);
    char buf[160];
    if (menuMode == MENU_ROMS) {
        drawText(1, 0, "PopUp16 - choose a game", C_HI);
        if (roms.empty()) {
            drawText(1, 2, "No ROMs found. Copy .sfc files to:", C_TEXT);
            drawText(1, 3, romDir.substr(0, COLS - 2), C_DIM);
        } else {
            int visible = ROWS - 3;
            int first = std::clamp(romSel - visible / 2, 0, std::max(0, (int)roms.size() - visible));
            for (int i = 0; i < visible && first + i < (int)roms.size(); i++) {
                int idx = first + i;
                std::string n = roms[idx].substr(0, roms[idx].find_last_of('.'));
                if ((int)n.size() > COLS - 4) n = n.substr(0, COLS - 4);
                bool sel = idx == romSel;
                if (sel) for (int x = 0; x < MENU_W; x++) for (int y = 0; y < CELL_H; y++) menuPixels[((i + 2) * CELL_H + y) * MENU_W + x] = C_SELBG;
                drawText(2, i + 2, n, sel ? 0xffffffff : C_TEXT);
            }
            snprintf(buf, sizeof buf, "%d/%d  stick: move  A/trigger: play  B: back", romSel + 1, (int)roms.size());
            drawText(1, ROWS - 1, buf, C_DIM);
        }
    } else if (menuMode == MENU_PAUSE) {
        std::string n = stem();
        if ((int)n.size() > COLS - 2) n = n.substr(0, COLS - 2);
        drawText(1, 0, n, C_HI);
        for (int i = 0; i < PAUSE_N; i++) {
            std::string v;
            switch (i) {
            case P_SPEED: snprintf(buf, sizeof buf, "%.2gx%s", cfg.speed, cfg.speed < 1 ? " slow-mo" : cfg.speed > 1 ? " fast" : ""); v = buf; break;
            case P_DEPTH: snprintf(buf, sizeof buf, "%.2f", cfg.strength); v = buf; break;
            case P_CONV: snprintf(buf, sizeof buf, "%+.1f", cfg.convergence); v = buf; break;
            case P_3D: v = on(cfg.stereoOn) ? "on" : "off"; break;
            case P_MODE7: v = on(cfg.mode7Ramp) ? "on" : "off"; break;
            case P_SWAP: v = on(cfg.swapEyes) ? "swapped" : "normal"; break;
            case P_ROOM: v = on(cfg.room) ? "your room" : "dark void"; break;
            case P_STYLE: v = on(cfg.box) ? "pop-up box" : "big screen"; break;
            case P_LOOK: v = on(cfg.popLook) ? "shadows + edges" : "classic"; break;
            case P_SKY: v = on(cfg.sky) ? "on" : "off (room behind)"; break;
            case P_SIZE: snprintf(buf, sizeof buf, "%.1f m", cfg.screenWidth); v = buf; break;
            case P_DIST: snprintf(buf, sizeof buf, "%.1f m", cfg.distance); v = buf; break;
            }
            bool sel = i == pauseSel;
            if (sel) for (int x = 0; x < MENU_W; x++) for (int y = 0; y < CELL_H; y++) menuPixels[((i + 2) * CELL_H + y) * MENU_W + x] = C_SELBG;
            drawText(2, i + 2, pauseItems[i], sel ? 0xffffffff : C_TEXT);
            bool arrows = adjustable(i) || toggle(i);
            if (!v.empty()) drawText(26, i + 2, (arrows ? "< " : "") + v + (arrows ? " >" : ""), sel ? 0xffffffff : C_TEXT);
        }
        drawText(1, ROWS - 1, "L stick: move  R stick: change  A: select  B: resume", C_DIM);
    }
    else if (menuMode == MENU_ABOUT) {
        for (int i = 0; i < ROWS - 1 && aboutTop + i < (int)aboutLines.size(); i++)
            drawText(1, i, aboutLines[aboutTop + i], aboutTop + i < 4 ? C_HI : C_TEXT);
        snprintf(buf, sizeof buf, "line %d/%d   L stick: scroll  R stick: page  B: back", aboutTop + 1, (int)aboutLines.size());
        drawText(1, ROWS - 1, buf, C_DIM);
    }
    else if (menuMode == MENU_SLOTS) {
        drawSlots();
    }
    else if (menuMode == MENU_ARRANGE) {
        static const char *lines[] = {"MOVE & RESIZE", "",
                                      "Hold a trigger and move your hand: the game follows,",
                                      "tilts and turns with it. Put it on a table or a wall.",
                                      "Hold both triggers and pull apart / push together",
                                      "to make it bigger or smaller.", "",
                                      "A or B: done.   Menu > Reset position puts it back."};
        for (int i = 0; i < 8; i++) drawText(1, i + 1, lines[i], i == 0 ? C_HI : C_TEXT);
    }
    else if (menuMode == MENU_HELP) {
        for (int i = 0; i < (int)(sizeof(helpLines) / sizeof(helpLines[0])) && i < ROWS; i++)
            drawText(1, i + 1, helpLines[i], i == 0 ? C_HI : C_TEXT);
    }
    if (!toast.empty() && nowSec() < toastUntil) drawText(1, ROWS - 2, toast, C_HI);
    menuDirty = false;
}
static void showToast(const std::string &t) { toast = t; toastUntil = nowSec() + 2.0; menuDirty = true; }

// ---------------------------------------------------------------- OpenXR / EGL
struct Swap {
    XrSwapchain handle = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};
static EGLDisplay eglDpy = EGL_NO_DISPLAY;
static EGLContext eglCtx = EGL_NO_CONTEXT;
static EGLSurface eglSurf = EGL_NO_SURFACE;
static XrInstance instance = XR_NULL_HANDLE;
static XrSystemId systemId = XR_NULL_SYSTEM_ID;
static XrSession session = XR_NULL_HANDLE;
static XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
static XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
static bool sessionRunning = false, hasRefreshExt = false, hasPassthrough = false;
static XrPassthroughFB passthrough = XR_NULL_HANDLE;
static XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
static bool passthroughRunning = false;
static Swap menuSwap;
static XrActionSet actionSet;
static XrAction actA, actB, actX, actY, actTrigL, actTrigR, actGripL, actGripR, actMenu, actStickL, actStickR, actClickL, actClickR;
static XrAction actPoseL, actPoseR;
static XrSpace handSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrPath handL, handR;

static XrPath path(const char *s) { XrPath p; xrStringToPath(instance, s, &p); return p; }

static bool initEGL() {
    eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDpy, nullptr, nullptr);
    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(eglDpy, attribs, &config, 1, &n) || n < 1) { LOGE("eglChooseConfig"); return false; }
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglCtx = eglCreateContext(eglDpy, config, EGL_NO_CONTEXT, ctxAttr);
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    eglSurf = eglCreatePbufferSurface(eglDpy, config, pb);
    if (eglCtx == EGL_NO_CONTEXT || !eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx)) { LOGE("egl context"); return false; }
    return true;
}

static bool makeSwap(Swap &s, int w, int h) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(session, n, &n, fmts.data());
    int64_t fmt = GL_RGBA8;
    for (int64_t f : fmts) if (f == GL_SRGB8_ALPHA8) fmt = f;  // pixel values are sRGB-encoded already
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session, &ci, &s.handle))) { LOGE("xrCreateSwapchain"); return false; }
    s.w = w; s.h = h;
    xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr);
    s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(s.handle, n, &n, (XrSwapchainImageBaseHeader *)s.images.data());
    return true;
}
static void uploadSwap(Swap &s, const uint32_t *pixels, int w, int h) {
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.handle, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.handle, &wi);
    // GL texture rows start at the bottom: upload bottom-up so the image is upright
    static std::vector<uint32_t> flipped;
    flipped.resize((size_t)w * h);
    for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w], pixels + (size_t)(h - 1 - y) * w, (size_t)w * 4);
    glBindTexture(GL_TEXTURE_2D, s.images[idx].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.handle, &ri);
}

static XrAction makeAction(const char *name, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy(ci.actionName, name); strcpy(ci.localizedActionName, name);
    XrPath subs[2] = {handL, handR};
    if (type == XR_ACTION_TYPE_FLOAT_INPUT) { ci.countSubactionPaths = 2; ci.subactionPaths = subs; }
    XrAction a; XRCHECK(xrCreateAction(actionSet, &ci, &a));
    return a;
}
static void initActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "snes"); strcpy(si.localizedActionSetName, "SNES");
    XRCHECK(xrCreateActionSet(instance, &si, &actionSet));
    handL = path("/user/hand/left"); handR = path("/user/hand/right");
    actA = makeAction("a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actB = makeAction("b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actX = makeAction("x", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actY = makeAction("y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actTrigL = makeAction("trigger_left", XR_ACTION_TYPE_FLOAT_INPUT);
    actTrigR = makeAction("trigger_right", XR_ACTION_TYPE_FLOAT_INPUT);
    actGripL = makeAction("grip_left", XR_ACTION_TYPE_FLOAT_INPUT);
    actGripR = makeAction("grip_right", XR_ACTION_TYPE_FLOAT_INPUT);
    actMenu = makeAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actStickL = makeAction("stick_left", XR_ACTION_TYPE_VECTOR2F_INPUT);
    actStickR = makeAction("stick_right", XR_ACTION_TYPE_VECTOR2F_INPUT);
    actClickL = makeAction("stick_click_left", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actClickR = makeAction("stick_click_right", XR_ACTION_TYPE_BOOLEAN_INPUT);
    actPoseL = makeAction("hand_left", XR_ACTION_TYPE_POSE_INPUT);
    actPoseR = makeAction("hand_right", XR_ACTION_TYPE_POSE_INPUT);
    std::vector<XrActionSuggestedBinding> b = {
        {actA, path("/user/hand/right/input/a/click")},
        {actB, path("/user/hand/right/input/b/click")},
        {actX, path("/user/hand/left/input/x/click")},
        {actY, path("/user/hand/left/input/y/click")},
        {actTrigL, path("/user/hand/left/input/trigger/value")},
        {actTrigR, path("/user/hand/right/input/trigger/value")},
        {actGripL, path("/user/hand/left/input/squeeze/value")},
        {actGripR, path("/user/hand/right/input/squeeze/value")},
        {actMenu, path("/user/hand/left/input/menu/click")},
        {actStickL, path("/user/hand/left/input/thumbstick")},
        {actStickR, path("/user/hand/right/input/thumbstick")},
        {actClickL, path("/user/hand/left/input/thumbstick/click")},
        {actClickR, path("/user/hand/right/input/thumbstick/click")},
        {actPoseL, path("/user/hand/left/input/grip/pose")},
        {actPoseR, path("/user/hand/right/input/grip/pose")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = (uint32_t)b.size(); sb.suggestedBindings = b.data();
    XRCHECK(xrSuggestInteractionProfileBindings(instance, &sb));
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1; ai.actionSets = &actionSet;
    XRCHECK(xrAttachSessionActionSets(session, &ai));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = h == 0 ? actPoseL : actPoseR;
        sci.poseInActionSpace.orientation.w = 1;
        XRCHECK(xrCreateActionSpace(session, &sci, &handSpace[h]));
    }
}
static bool getBool(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &s)) && s.isActive && s.currentState;
}
static float getFloat(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
}
static XrVector2f getStick(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &s)) && s.isActive) return s.currentState;
    return {0, 0};
}
static void pollActions() {
    XrActiveActionSet as{actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &as;
    XrResult sr0 = xrSyncActions(session, &si);
    static XrResult lastSync = XR_SUCCESS;
    if (sr0 != lastSync) { trace("xrSyncActions -> %d", (int)sr0); lastSync = sr0; }
    {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = actA;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session, &gi, &st);
        static int lastActive = -1;
        if ((int)st.isActive != lastActive) { trace("controller actions %s", st.isActive ? "active" : "INACTIVE"); lastActive = st.isActive; }
    }
    if (XR_FAILED(sr0)) return;
    XrVector2f sl = getStick(actStickL), sr = getStick(actStickR);
    float sx = fabsf(sl.x) > fabsf(sr.x) ? sl.x : sr.x, sy = fabsf(sl.y) > fabsf(sr.y) ? sl.y : sr.y;
    navX = sr.x; navY = sl.y;  // menu: left stick moves up/down, right stick changes values / pages
    trigRValue = getFloat(actTrigR);
    memset(xrButtons, 0, sizeof xrButtons);
    xrButtons[B_B] = getBool(actA);
    xrButtons[B_A] = getBool(actB);
    xrButtons[B_Y] = getFloat(actTrigR) > 0.5f;
    xrButtons[B_X] = getFloat(actTrigL) > 0.5f;
    xrButtons[B_L] = getFloat(actGripL) > 0.5f;
    xrButtons[B_R] = getFloat(actGripR) > 0.5f;
    xrButtons[B_SELECT] = getBool(actX);
    xrRewind = getBool(actClickL);  // hold the left stick in to rewind
    xrButtons[B_START] = getBool(actMenu) || getBool(actClickR);
    xrButtons[B_UP] = sy > 0.5f; xrButtons[B_DOWN] = sy < -0.5f;
    xrButtons[B_LEFT] = sx < -0.5f; xrButtons[B_RIGHT] = sx > 0.5f;
    xrMenu = getBool(actY);
}

static bool initXR(android_app *app) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = app->activity->vm; li.applicationContext = app->activity->clazz;
        initLoader((XrLoaderInitInfoBaseHeaderKHR *)&li);
    }
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    for (auto &p : props) {
        if (!strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) hasRefreshExt = true;
        if (!strcmp(p.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME)) hasPassthrough = true;
    }
    std::vector<const char *> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    if (hasRefreshExt) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (hasPassthrough) exts.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR ia{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ia.applicationVM = app->activity->vm; ia.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ia;
    strcpy(ci.applicationInfo.applicationName, "PopUp16");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.enabledExtensionNames = exts.data();
    if (XR_FAILED(xrCreateInstance(&ci, &instance))) { LOGE("xrCreateInstance failed"); return false; }

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO}; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(instance, &sg, &systemId))) { LOGE("xrGetSystem failed"); return false; }

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(instance, systemId, &req);

    if (!initEGL()) return false;
    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = eglDpy; gb.config = nullptr; gb.context = eglCtx;
    EGLint cfgId = 0; eglQueryContext(eglDpy, eglCtx, EGL_CONFIG_ID, &cfgId);
    EGLint ca[] = {EGL_CONFIG_ID, cfgId, EGL_NONE}; EGLint cn = 0; EGLConfig ec;
    if (eglChooseConfig(eglDpy, ca, &ec, 1, &cn) && cn == 1) gb.config = ec;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb; sci.systemId = systemId;
    if (XR_FAILED(xrCreateSession(instance, &sci, &session))) { LOGE("xrCreateSession failed"); return false; }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRCHECK(xrCreateReferenceSpace(session, &rs, &localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHECK(xrCreateReferenceSpace(session, &rs, &viewSpace));

    if (!makeSwap(menuSwap, MENU_W, MENU_H)) return false;
    {
        uint32_t vc = 0;
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
        std::vector<XrViewConfigurationView> vcv(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, vcv.data());
        if (vc < 2 || !renderer.init()) { LOGE("renderer init failed"); return false; }
        for (int eye = 0; eye < 2; eye++) {
            Swap s;
            if (!makeSwap(s, (int)vcv[eye].recommendedImageRectWidth, (int)vcv[eye].recommendedImageRectHeight)) return false;
            render::Eye &e = renderer.eyes[eye];
            e.swap = s.handle; e.w = s.w; e.h = s.h; e.images = s.images;
            e.fbos.assign(e.images.size(), 0);
            glGenRenderbuffers(1, &e.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, e.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e.w, e.h);
            LOGI("eye %d: %dx%d", eye, e.w, e.h);
        }
    }
    initActions();
    return true;
}

// room passthrough (XR_FB_passthrough): created once, started only while "your room" is chosen
static void setPassthrough(bool want) {
    if (!hasPassthrough || want == passthroughRunning) return;
    static PFN_xrCreatePassthroughFB createPt;
    static PFN_xrCreatePassthroughLayerFB createLayer;
    static PFN_xrPassthroughStartFB startPt;
    static PFN_xrPassthroughPauseFB pausePt;
    static PFN_xrPassthroughLayerResumeFB resumeLayer;
    static PFN_xrPassthroughLayerPauseFB pauseLayer;
    if (!createPt) {
        xrGetInstanceProcAddr(instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&createPt);
        xrGetInstanceProcAddr(instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&createLayer);
        xrGetInstanceProcAddr(instance, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&startPt);
        xrGetInstanceProcAddr(instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&pausePt);
        xrGetInstanceProcAddr(instance, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction *)&resumeLayer);
        xrGetInstanceProcAddr(instance, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction *)&pauseLayer);
        if (!createPt || !createLayer || !startPt || !pausePt || !resumeLayer || !pauseLayer) { hasPassthrough = false; return; }
    }
    if (want) {
        if (!passthrough) {
            XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            if (XR_FAILED(createPt(session, &pci, &passthrough))) { trace("passthrough unavailable"); hasPassthrough = false; return; }
            XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = passthrough;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (XR_FAILED(createLayer(session, &lci, &passthroughLayer))) { trace("passthrough layer failed"); hasPassthrough = false; return; }
        }
        startPt(passthrough);
        resumeLayer(passthroughLayer);
    } else {
        pauseLayer(passthroughLayer);
        pausePt(passthrough);
    }
    passthroughRunning = want;
    trace("passthrough %s", want ? "on" : "off");
}

static void requestRefreshRate() {
    if (!hasRefreshExt) return;
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumRates);
    xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&requestRate);
    if (!enumRates || !requestRate) return;
    uint32_t n = 0;
    enumRates(session, 0, &n, nullptr);
    std::vector<float> rates(n);
    enumRates(session, n, &n, rates.data());
    float best = 0;
    for (float r : rates) if (fabsf(r - 120.0f) < 0.5f) best = r;  // 120 Hz shows 60 fps games without judder
    if (best == 0) for (float r : rates) if (fabsf(r - 90.0f) > 0.5f && r > best) best = r;
    if (best > 0) { requestRate(session, best); LOGI("display refresh %.0f Hz", best); }
}

static void handleXrEvents(android_app *app) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *sc = (XrEventDataSessionStateChanged *)&ev;
            sessionState = sc->state;
            trace("session state %d", (int)sessionState);
            if (sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session, &bi))) { sessionRunning = true; requestRefreshRate(); }
            } else if (sessionState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session); sessionRunning = false; saveSram(); saveResume();
            } else if (sessionState == XR_SESSION_STATE_EXITING || sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                ANativeActivity_finish(app->activity);
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            ANativeActivity_finish(app->activity);
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------- menu input
// Menu navigation is deliberately calm: left stick up/down picks a row, right stick left/right
// changes a value (or pages the game list). A stick must pass 70% to register and come back under
// 30% before the next step; value changes never auto-repeat; list scrolling repeats slowly.
enum Dir { D_NONE, D_UP, D_DOWN, D_LEFT, D_RIGHT };
static Dir navDir = D_NONE;
static double navRepeatAt = 0;
static bool okHeld = true, backHeld = true, trigHeld = true;

static Dir readDir(const bool *b, float x, float y) {
    if (b[B_UP]) return D_UP;
    if (b[B_DOWN]) return D_DOWN;
    if (b[B_LEFT]) return D_LEFT;
    if (b[B_RIGHT]) return D_RIGHT;
    if (navDir != D_NONE && fabsf(x) > 0.3f && (navDir == D_LEFT || navDir == D_RIGHT)) return navDir;
    if (navDir != D_NONE && fabsf(y) > 0.3f && (navDir == D_UP || navDir == D_DOWN)) return navDir;
    if (fabsf(y) > 0.7f && fabsf(y) >= fabsf(x)) return y > 0 ? D_UP : D_DOWN;
    if (fabsf(x) > 0.7f) return x > 0 ? D_RIGHT : D_LEFT;
    return D_NONE;
}
static void resetMenuInput() { navDir = D_NONE; okHeld = backHeld = trigHeld = true; }


static void menuInput(const bool *b) {
    // the stick reads "up" as +y; gamepad axes read "up" as -y (already folded into b[] as digital)
    Dir d = readDir(b, navX, navY);
    double t = nowSec();
    bool fire = false;
    if (d != navDir) { fire = d != D_NONE; navRepeatAt = t + 0.6; }
    else if (d == D_UP || d == D_DOWN) { if (t >= navRepeatAt) { fire = true; navRepeatAt = t + 0.2; } }
    navDir = d;
    bool up = fire && d == D_UP, down = fire && d == D_DOWN, left = fire && d == D_LEFT, right = fire && d == D_RIGHT;
    // buttons fire once per press; the trigger needs a firm pull and a full release
    bool okBtn = b[B_B] || padButtons[B_Y];
    if (menuMode == MENU_ARRANGE) {  // triggers are busy grabbing; only A / B leave
        bool done = (okBtn && !okHeld) || (b[B_A] && !backHeld);
        okHeld = okBtn; backHeld = b[B_A];
        if (done) { menuMode = MENU_PAUSE; saveGlobal(); menuDirty = true; }
        return;
    }
    bool ok = okBtn && !okHeld;
    okHeld = okBtn;
    if (trigRValue > 0.85f && !trigHeld) ok = true;
    trigHeld = trigRValue > 0.3f ? (trigHeld || trigRValue > 0.85f) : false;
    bool back = b[B_A] && !backHeld;
    backHeld = b[B_A];
    if (menuMode == MENU_HELP) {
        if (ok || back || fire) { menuMode = MENU_NONE; menuDirty = true; }
        return;
    }
    if (menuMode == MENU_SLOTS) {
        if (up || down) slotSel ^= 2;
        if (left || right) slotSel ^= 1;
        if (back) menuMode = MENU_PAUSE;
        if (ok) {
            if (slotSaving) saveSlot(slotSel);
            else if (loadSlot(slotSel)) { menuMode = MENU_NONE; rewinder.clear(); }
            else showToast("Slot " + std::to_string(slotSel + 1) + " is empty");
        }
        menuDirty |= up || down || left || right || back || ok;
        return;
    }
    if (menuMode == MENU_ABOUT) {
        int page = ROWS - 2, last = std::max(0, (int)aboutLines.size() - page);
        if (up) aboutTop = std::max(0, aboutTop - 1);
        if (down) aboutTop = std::min(last, aboutTop + 1);
        if (left) aboutTop = std::max(0, aboutTop - page);
        if (right) aboutTop = std::min(last, aboutTop + page);
        if (back || ok) menuMode = MENU_PAUSE;
        menuDirty |= up || down || left || right || back || ok;
        return;
    }
    if (menuMode == MENU_ROMS) {
        if (roms.empty()) { if (ok) scanRoms(); menuDirty |= ok; return; }
        int page = ROWS - 3;
        if (up) romSel = (romSel + (int)roms.size() - 1) % (int)roms.size();
        if (down) romSel = (romSel + 1) % (int)roms.size();
        if (left) romSel = std::max(0, romSel - page);
        if (right) romSel = std::min((int)roms.size() - 1, romSel + page);
        if (ok) {
            if (loadGame(roms[romSel])) { menuMode = MENU_HELP; resetMenuInput(); }
            else showToast("Could not load that ROM");
        }
        if (back && gameLoaded) menuMode = MENU_PAUSE;
        menuDirty |= up || down || left || right || ok || back;
        return;
    }
    // pause menu
    if (up) pauseSel = (pauseSel + PAUSE_N - 1) % PAUSE_N;
    if (down) pauseSel = (pauseSel + 1) % PAUSE_N;
    int delta = right ? 1 : left ? -1 : 0;
    bool changed = false;
    if (delta || (ok && toggle(pauseSel))) {
        changed = true;
        switch (pauseSel) {
        case P_SPEED: {
            static const float speeds[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
            int k = 2;
            for (int j = 0; j < 5; j++) if (fabsf(speeds[j] - cfg.speed) < 0.01f) k = j;
            cfg.speed = speeds[std::clamp(k + delta, 0, 4)];
            break;
        }
        case P_DEPTH: cfg.strength = std::clamp(cfg.strength + 0.25f * delta, 0.0f, 3.0f); break;
        case P_CONV: cfg.convergence += 0.5f * delta; break;
        case P_3D: flip(cfg.stereoOn); break;
        case P_MODE7: flip(cfg.mode7Ramp); break;
        case P_SWAP: flip(cfg.swapEyes); break;
        case P_ROOM: flip(cfg.room); break;
        case P_STYLE: flip(cfg.box); break;
        case P_LOOK: flip(cfg.popLook); break;
        case P_SKY: flip(cfg.sky); break;
        case P_SIZE: cfg.screenWidth = std::clamp(cfg.screenWidth + 0.2f * delta, 0.3f, 8.0f); break;
        case P_DIST: {
            cfg.distance = std::clamp(cfg.distance + 0.2f * delta, 0.4f, 8.0f);
            float len = sqrtf(cfg.px * cfg.px + cfg.py * cfg.py + cfg.pz * cfg.pz);
            if (len < 0.01f) defaultPlacement();
            else { float k = cfg.distance / len; cfg.px *= k; cfg.py *= k; cfg.pz *= k; }
            break;
        }
        default: changed = false;
        }
        if (changed) { newFrame = frame.valid; saveGlobal(); }  // rebuild the paused frame with the new settings
    }
    if (ok && !toggle(pauseSel)) {
        switch (pauseSel) {
        case P_RESUME: menuMode = MENU_NONE; break;
        case P_CONTROLS: menuMode = MENU_HELP; break;
        case P_ARRANGE: menuMode = MENU_ARRANGE; resetMenuInput(); break;
        case P_RESET: defaultPlacement(); saveGlobal(); showToast("Position reset"); break;
        case P_SAVE: menuMode = MENU_SLOTS; slotSaving = true; break;
        case P_LOAD: menuMode = MENU_SLOTS; slotSaving = false; break;
        case P_GAMES: scanRoms(); menuMode = MENU_ROMS; break;
        case P_ABOUT: menuMode = MENU_ABOUT; aboutTop = 0; break;
        }
    }
    if (back) menuMode = MENU_NONE;
    menuDirty |= up || down || delta || ok || back || changed;
}

// ---------------------------------------------------------------- placement
struct Quat { float x, y, z, w; };
static Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat qconj(Quat a) { return {-a.x, -a.y, -a.z, a.w}; }
static XrVector3f qrot(Quat q, XrVector3f v) {
    Quat r = qmul(qmul(q, {v.x, v.y, v.z, 0}), qconj(q));
    return {r.x, r.y, r.z};
}
static Quat qnorm(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{0, 0, 0, 1};
}
static void placementMatrix(float *m) {  // column-major: translate(p) * rotate(q)
    Quat q = qnorm({cfg.qx, cfg.qy, cfg.qz, cfg.qw});
    XrVector3f cx = qrot(q, {1, 0, 0}), cy = qrot(q, {0, 1, 0}), cz = qrot(q, {0, 0, 1});
    float M[16] = {cx.x, cx.y, cx.z, 0, cy.x, cy.y, cy.z, 0, cz.x, cz.y, cz.z, 0, cfg.px, cfg.py, cfg.pz, 1};
    memcpy(m, M, sizeof M);
}

// Move & resize: hold one trigger to carry the game with that hand (position and tilt);
// hold both triggers and pull apart / push together to resize.
static struct {
    int hand = -1;                 // hand carrying it, -1 none, 2 both
    XrPosef start[2];
    XrVector3f pos0; Quat rot0; float width0 = 0, span0 = 0;
} grab;
static void arrangeUpdate(XrTime t) {
    XrSpaceLocation loc[2] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    bool valid[2];
    for (int h = 0; h < 2; h++) {
        valid[h] = XR_SUCCEEDED(xrLocateSpace(handSpace[h], localSpace, t, &loc[h])) &&
                   (loc[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                   (loc[h].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    }
    bool held[2] = {valid[0] && getFloat(actTrigL) > 0.6f, valid[1] && getFloat(actTrigR) > 0.6f};
    int mode = held[0] && held[1] ? 2 : held[0] ? 0 : held[1] ? 1 : -1;
    if (mode != grab.hand) {  // (re)start from the current state whenever the grip changes
        grab.hand = mode;
        for (int h = 0; h < 2; h++) grab.start[h] = loc[h].pose;
        grab.pos0 = {cfg.px, cfg.py, cfg.pz};
        grab.rot0 = qnorm({cfg.qx, cfg.qy, cfg.qz, cfg.qw});
        grab.width0 = cfg.screenWidth;
        XrVector3f a = loc[0].pose.position, b = loc[1].pose.position;
        grab.span0 = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        if (mode < 0) saveGlobal();
        return;
    }
    if (mode < 0) return;
    if (mode == 2) {
        XrVector3f a = loc[0].pose.position, b = loc[1].pose.position;
        XrVector3f a0 = grab.start[0].position, b0 = grab.start[1].position;
        float span = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        if (grab.span0 > 0.02f) cfg.screenWidth = std::clamp(grab.width0 * span / grab.span0, 0.3f, 8.0f);
        cfg.px = grab.pos0.x + 0.5f * ((a.x + b.x) - (a0.x + b0.x));
        cfg.py = grab.pos0.y + 0.5f * ((a.y + b.y) - (a0.y + b0.y));
        cfg.pz = grab.pos0.z + 0.5f * ((a.z + b.z) - (a0.z + b0.z));
    } else {
        const XrPosef &now = loc[mode].pose, &st = grab.start[mode];
        Quat qn = {now.orientation.x, now.orientation.y, now.orientation.z, now.orientation.w};
        Quat qs = {st.orientation.x, st.orientation.y, st.orientation.z, st.orientation.w};
        Quat dq = qnorm(qmul(qn, qconj(qs)));                      // how the hand turned since grabbing
        XrVector3f rel = {grab.pos0.x - st.position.x, grab.pos0.y - st.position.y, grab.pos0.z - st.position.z};
        XrVector3f r = qrot(dq, rel);
        cfg.px = now.position.x + r.x; cfg.py = now.position.y + r.y; cfg.pz = now.position.z + r.z;
        Quat nr = qnorm(qmul(dq, grab.rot0));
        cfg.qx = nr.x; cfg.qy = nr.y; cfg.qz = nr.z; cfg.qw = nr.w;
    }
    cfg.distance = std::clamp(sqrtf(cfg.px * cfg.px + cfg.py * cfg.py + cfg.pz * cfg.pz), 0.4f, 8.0f);
}

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
                      "Emulation by the Snes9x team. No games included.", "", ""};
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
    scanRoms();

    if (!initXR(app)) { LOGE("OpenXR init failed"); ANativeActivity_finish(app->activity); }
    startAudio();
    worker.start();
    {   // test hook: files/autostart.txt names a ROM to load immediately (file is consumed)
        std::vector<uint8_t> a;
        if (readFile(filesDir + "/autostart.txt", a)) {
            std::string n(a.begin(), a.end());
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
            remove((filesDir + "/autostart.txt").c_str());
            if (loadGame(n)) menuMode = MENU_NONE;
        } else if (readFile(filesDir + "/last_game.txt", a)) {
            // quick resume: reopen the last game exactly where it was left
            std::string n(a.begin(), a.end());
            std::vector<uint8_t> st;
            if (loadGame(n)) {
                if (readFile(saveDir + "/" + stem() + ".resume", st)) retro_unserialize(st.data(), st.size());
                menuMode = MENU_NONE;
                trace("resumed %s", n.c_str());
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
            static bool headless = access((filesDir + "/debug_headless").c_str(), F_OK) == 0;
            if (headless && gameLoaded && menuMode == MENU_NONE) {
                bool in[B_COUNT];
                for (int i = 0; i < B_COUNT; i++) in[i] = padButtons[i];
                in[B_UP] |= padAxisY < -0.5f; in[B_DOWN] |= padAxisY > 0.5f;
                in[B_LEFT] |= padAxisX < -0.5f; in[B_RIGHT] |= padAxisX > 0.5f;
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
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) continue;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        pollActions();
        bool all[B_COUNT];
        for (int i = 0; i < B_COUNT; i++) all[i] = xrButtons[i] || padButtons[i];
        all[B_UP] |= padAxisY < -0.5f; all[B_DOWN] |= padAxisY > 0.5f;
        all[B_LEFT] |= padAxisX < -0.5f; all[B_RIGHT] |= padAxisX > 0.5f;
        {
            static const char *names[B_COUNT] = {"B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R"};
            static bool last[B_COUNT];
            static bool lastMenu;
            for (int i = 0; i < B_COUNT; i++)
                if (all[i] != last[i]) { trace("input SNES %s %s (menu mode %d, game %d)", names[i], all[i] ? "down" : "up", (int)menuMode, (int)gameLoaded); last[i] = all[i]; }
            if (xrMenu != lastMenu) { trace("input PopUp16-menu button %s", xrMenu ? "down" : "up"); lastMenu = xrMenu; }
        }
        bool menuBtn = xrMenu || padMenu || (padButtons[B_SELECT] && padButtons[B_START]);
        if (menuBtn && !prevMenuBtn) {
            if (menuMode == MENU_NONE) { menuMode = MENU_PAUSE; pauseSel = 0; saveSram(); }
            else if (gameLoaded) { menuMode = MENU_NONE; saveGlobal(); saveSettings(saveDir + "/" + stem() + ".cfg", false); }
            menuDirty = true;
            resetMenuInput();  // swallow whatever is held when the menu opens
        }
        prevMenuBtn = menuBtn;

        if (menuMode != MENU_NONE) {
            memset(joypad, 0, sizeof joypad);
            MenuMode before = menuMode;
            menuInput(all);
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
            bool rewinding = xrRewind || padRewind;
            // game speed: the emulator follows the audio clock, so stretching the audio slows the game
            float speed = std::clamp(cfg.speed * (padFast ? 2.0f : 1.0f), 0.25f, 4.0f);
            resampleStep = avRate * speed / OUT_RATE;
            if (rewinding) {
                double t = nowSec();
                if (t - lastEmu > 0.25) lastEmu = t;
                while (t - lastEmu >= 1.0 / avFps && runs < 2) { rewindStep(); runs++; lastEmu += 1.0 / avFps; }
            } else if (aaStream) { while (ringFill() < target && runs < 4) { runFrame(); runs++; } lastEmu = nowSec(); }
            else { double t = nowSec(); while (t - lastEmu >= 1.0 / (avFps * speed) && runs < 4) { runFrame(); runs++; lastEmu += 1.0 / (avFps * speed); } }
            statRuns += runs;
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
                worker.hasJob = true;
            }
            worker.cv.notify_one();
            newFrame = false;
        }
        {   // upload the newest finished build
            int r;
            { std::lock_guard<std::mutex> l(worker.m); r = worker.ready; worker.ready = -1; worker.uploading = r; }
            if (r >= 0) {
                diorama::Builder &b = worker.builders[r];
                renderer.haze = b.look.on ? b.look.haze : 0.0f;
                renderer.upload(b, b.lastW, b.lastH);
                std::lock_guard<std::mutex> l(worker.m);
                worker.uploading = -1;
            }
        }
        if (menuMode != MENU_NONE && (menuDirty || (!toast.empty() && nowSec() > toastUntil))) {
            if (nowSec() > toastUntil) toast.clear();
            renderMenu();
            uploadSwap(menuSwap, menuPixels.data(), MENU_W, MENU_H);
        }

        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        if (menuMode == MENU_ARRANGE) arrangeUpdate(fs.predictedDisplayTime);
        setPassthrough(on(cfg.room) && hasPassthrough);
        XrCompositionLayerPassthroughFB ptLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        XrCompositionLayerQuad quads[1];
        const XrCompositionLayerBaseHeader *layers[3];
        uint32_t nl = 0;
        bool roomVisible = passthroughRunning && fs.shouldRender;
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
            float viewDist = std::max(0.4f, sqrtf(cfg.px * cfg.px + cfg.py * cfg.py + cfg.pz * cfg.pz));
            float screen[4] = {cfg.screenWidth, cfg.screenWidth * 3.0f / 4.0f, viewDist, pxM};
            float model[16];
            placementMatrix(model);
            // box style: a full-depth stack is about a third of the width deep, whatever the comfort scale
            float style[2] = {on(cfg.box) ? 1.0f : 0.0f, 0.04f * cfg.screenWidth / std::max(autoScale, 1e-3f)};
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
        if (fs.shouldRender && menuMode != MENU_NONE) {
            XrCompositionLayerQuad &q = quads[0];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.space = localSpace;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = menuSwap.handle;
            q.subImage.imageRect = {{0, 0}, {MENU_W, MENU_H}};
            q.pose.orientation.w = 1;
            if (menuMode == MENU_ARRANGE) {  // small hint card low in view, out of the way of the game
                q.pose.position = {0.0f, -0.55f, -1.0f};
                q.size = {0.6f, 0.6f * MENU_H / MENU_W};
            } else {
                q.pose.position = {0.0f, 0.0f, -1.4f};
                q.size = {1.2f, 1.2f * MENU_H / MENU_W};
            }
            layers[nl] = (XrCompositionLayerBaseHeader *)&q;
            nl++;
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
        xrEndFrame(session, &fei);
    }

    worker.stop();
    unloadGame();
    saveGlobal();
    stopAudio();
    if (session) xrDestroySession(session);
    if (instance) xrDestroyInstance(instance);
    app->activity->vm->DetachCurrentThread();
}
