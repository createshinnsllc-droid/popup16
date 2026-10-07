// PopUp16: stereoscopic side-by-side SNES frontend for a patched snes9x libretro core.
// The core exports a per-pixel layer id (BG1-4, sprites, backdrop, Mode 7) and priority depth.
// Each layer gets a disparity; every frame is forward-warped once per eye and shown side by side,
// ready for Virtual Desktop's SBS 3D mode on a Quest headset.

#include <SDL.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "libretro.h"
#include "../shared/stereo.h"
#include "../shared/diorama.h"
#include "../shared/rewind.h"
#include "../shared/png.h"

// ---------- core binding ----------
struct Core {
    void *h = nullptr;
    void (*init)(void);
    void (*deinit)(void);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*get_system_av_info)(retro_system_av_info *);
    bool (*load_game)(const retro_game_info *);
    void (*unload_game)(void);
    void (*run)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    const uint8_t *(*get_layers)(void);
    const uint8_t *(*get_depths)(void);
} core;

template <class T> static void sym(T &fn, const char *name) {
    fn = (T)dlsym(core.h, name);
    if (!fn) { fprintf(stderr, "core missing symbol %s\n", name); exit(1); }
}

// ---------- settings ----------
struct Profile {
    float strength = 1.0f;     // depth multiplier
    float convergence = 0.0f;  // added to every disparity (SNES px); + pushes scene into the screen
    int halfSbs = 1;           // 1: each eye squeezed into half width (VD "SBS"), 0: full aspect per half
    int mode7Ramp = 1;         // Mode 7 floor gets a near-to-far ramp
    int swapEyes = 0;
    float aspect = 4.0f / 3.0f;
};
static Profile prof;
static bool stereoOn = true;
static std::string profilePath, sramPath, statePath;

static void saveProfile() {
    FILE *f = fopen(profilePath.c_str(), "w");
    if (!f) return;
    fprintf(f, "strength=%g\nconvergence=%g\nhalfSbs=%d\nmode7Ramp=%d\nswapEyes=%d\naspect=%g\n",
            prof.strength, prof.convergence, prof.halfSbs, prof.mode7Ramp, prof.swapEyes, prof.aspect);
    fclose(f);
}
static void loadProfile() {
    FILE *f = fopen(profilePath.c_str(), "r");
    if (!f) return;
    char k[64]; float v;
    while (fscanf(f, "%63[^=]=%f\n", k, &v) == 2) {
        std::string s = k;
        if (s == "strength") prof.strength = v;
        else if (s == "convergence") prof.convergence = v;
        else if (s == "halfSbs") prof.halfSbs = (int)v;
        else if (s == "mode7Ramp") prof.mode7Ramp = (int)v;
        else if (s == "swapEyes") prof.swapEyes = (int)v;
        else if (s == "aspect") prof.aspect = v;
    }
    fclose(f);
}

// ---------- video ----------
using stereo::EYE_W;
static uint32_t lut565[65536];
static stereo::Frame cur;
static std::vector<uint32_t> sbs;        // (2*EYE_W) x outH
static unsigned outH = 0;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;  // dupe
    cur.capture(data, w, h, pitch, core.get_layers ? core.get_layers() : nullptr,
                core.get_depths ? core.get_depths() : nullptr);
}

static void buildSbs() {
    stereo::Params sp;
    sp.strength = prof.strength; sp.convergence = prof.convergence;
    sp.enabled = stereoOn; sp.mode7Ramp = prof.mode7Ramp; sp.swapEyes = prof.swapEyes;
    outH = stereo::build(cur, sp, lut565, sbs);
}

// ---------- audio / input ----------
static SDL_AudioDeviceID audioDev = 0;
static void audio_sample(int16_t l, int16_t r) {
    int16_t s[2] = {l, r};
    if (audioDev) SDL_QueueAudio(audioDev, s, 4);
}
static size_t audio_batch(const int16_t *d, size_t frames) {
    if (audioDev) SDL_QueueAudio(audioDev, d, (Uint32)(frames * 4));
    return frames;
}

static SDL_GameController *pads[2] = {nullptr, nullptr};
static const Uint8 *keys = nullptr;
static void input_poll(void) {}
static int autoFrame = -1;  // test mode: scripted presses (Start, then A, holding Right)
static int16_t input_state(unsigned port, unsigned device, unsigned, unsigned id) {
    if (device != RETRO_DEVICE_JOYPAD || port > 1) return 0;
    if (autoFrame >= 0) {
        if (port != 0) return 0;
        int t = autoFrame % 150;
        if (id == RETRO_DEVICE_ID_JOYPAD_START) return t < 5;
        if (id == RETRO_DEVICE_ID_JOYPAD_A || id == RETRO_DEVICE_ID_JOYPAD_B) return t >= 75 && t < 80;
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT) return autoFrame > 900;
        return 0;
    }
    bool on = false;
    SDL_GameController *p = pads[port];
    if (p) {
        // SNES layout: SDL A (bottom) = SNES B, SDL B (right) = SNES A, SDL X (left) = SNES Y, SDL Y (top) = SNES X
        Sint16 lx = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ly = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
        const int dz = 16000;
        auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(p, btn) != 0; };
        switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B: on = b(SDL_CONTROLLER_BUTTON_A); break;
        case RETRO_DEVICE_ID_JOYPAD_A: on = b(SDL_CONTROLLER_BUTTON_B); break;
        case RETRO_DEVICE_ID_JOYPAD_Y: on = b(SDL_CONTROLLER_BUTTON_X); break;
        case RETRO_DEVICE_ID_JOYPAD_X: on = b(SDL_CONTROLLER_BUTTON_Y); break;
        case RETRO_DEVICE_ID_JOYPAD_L: on = b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER); break;
        case RETRO_DEVICE_ID_JOYPAD_R: on = b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); break;
        case RETRO_DEVICE_ID_JOYPAD_START: on = b(SDL_CONTROLLER_BUTTON_START); break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: on = b(SDL_CONTROLLER_BUTTON_BACK); break;
        case RETRO_DEVICE_ID_JOYPAD_UP: on = b(SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -dz; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN: on = b(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > dz; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT: on = b(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -dz; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: on = b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > dz; break;
        }
    }
    if (port == 0 && keys && !on) {
        switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_B: on = keys[SDL_SCANCODE_Z]; break;
        case RETRO_DEVICE_ID_JOYPAD_A: on = keys[SDL_SCANCODE_X]; break;
        case RETRO_DEVICE_ID_JOYPAD_Y: on = keys[SDL_SCANCODE_A]; break;
        case RETRO_DEVICE_ID_JOYPAD_X: on = keys[SDL_SCANCODE_S]; break;
        case RETRO_DEVICE_ID_JOYPAD_L: on = keys[SDL_SCANCODE_Q]; break;
        case RETRO_DEVICE_ID_JOYPAD_R: on = keys[SDL_SCANCODE_W]; break;
        case RETRO_DEVICE_ID_JOYPAD_START: on = keys[SDL_SCANCODE_RETURN]; break;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: on = keys[SDL_SCANCODE_RSHIFT]; break;
        case RETRO_DEVICE_ID_JOYPAD_UP: on = keys[SDL_SCANCODE_UP]; break;
        case RETRO_DEVICE_ID_JOYPAD_DOWN: on = keys[SDL_SCANCODE_DOWN]; break;
        case RETRO_DEVICE_ID_JOYPAD_LEFT: on = keys[SDL_SCANCODE_LEFT]; break;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT: on = keys[SDL_SCANCODE_RIGHT]; break;
        }
    }
    return on ? 1 : 0;
}

static void openPads() {
    for (int i = 0, n = 0; i < SDL_NumJoysticks() && n < 2; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(i);
        if (!c) continue;
        if (pads[n]) SDL_GameControllerClose(pads[n]);
        pads[n++] = c;
    }
}

// ---------- environment ----------
static std::string sysDir, saveDir;
static void core_log(enum retro_log_level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = sysDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = saveDir.c_str(); return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    default: return false;
    }
}

// ---------- helpers ----------
static bool readFile(const std::string &p, std::vector<uint8_t> &out) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n);
    bool ok = fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}
static bool writeFile(const std::string &p, const void *d, size_t n) {
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", p.c_str()); return false; }
    bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "incomplete write %s\n", p.c_str());
    return ok;
}
static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower(c); return s; }
static std::string shq(const std::string &s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}
static std::string sevenZip() {
    for (const char *c : {"/opt/homebrew/bin/7zz", "/opt/homebrew/bin/7z", "/usr/local/bin/7zz", "/usr/local/bin/7z"})
        if (access(c, X_OK) == 0) return c;
    return "";
}
// GoodSNES naming: prefer verified [!] dumps, US > EU > JP, English translations of JP-only games;
// avoid bad dumps, hacks, fixes, trainers, overdumps, betas.
static int scoreDump(const std::string &n) {
    std::string l = lower(n);
    if (!(l.size() > 4 && (l.ends_with(".sfc") || l.ends_with(".smc") || l.ends_with(".swc") || l.ends_with(".fig")))) return -100000;
    int s = 0;
    if (n.find("[!]") != std::string::npos) s += 100;
    if (n.find("(U)") != std::string::npos || n.find("(JU)") != std::string::npos || n.find("(UE)") != std::string::npos) s += 40;
    else if (n.find("(E)") != std::string::npos) s += 25;
    else if (n.find("(J)") != std::string::npos) s += 10;
    if (n.find("[T+Eng") != std::string::npos) s += 35;
    else if (n.find("[T") != std::string::npos) s -= 20;
    for (const char *bad : {"[b", "[h", "[f", "[t", "[o", "[p", "[x"}) if (n.find(bad) != std::string::npos) s -= 80;
    if (n.find("[a") != std::string::npos) s -= 5;
    for (const char *bad : {"(beta", "(proto", "(sample", "(demo", "competition", "(hack", "(pd)"}) if (l.find(bad) != std::string::npos) s -= 60;
    size_t v = n.find("(V1.");
    if (v != std::string::npos && v + 4 < n.size() && isdigit((unsigned char)n[v + 4])) s += n[v + 4] - '0';
    return s;
}

static void updateTitle(SDL_Window *w, const std::string &name, const char *msg = nullptr) {
    char t[512];
    snprintf(t, sizeof t, "PopUp16 - %s | 3D %s  depth %.2f  conv %+.1f  %s%s%s",
             name.c_str(), stereoOn ? "on" : "off", prof.strength, prof.convergence,
             prof.halfSbs ? "half-SBS" : "full-SBS", msg ? "  | " : "", msg ? msg : "");
    SDL_SetWindowTitle(w, t);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: popup16 <snes9x_libretro.dylib> <rom.sfc|.smc|.zip> [--window]\n");
        return 1;
    }
    std::string corePath = argv[1], romPath = argv[2];
    bool startWindowed = argc > 3 && std::string(argv[3]) == "--window";
    // --dump <frames> <out.bmp>: headless run, writes the side-by-side frame and exits (for testing)
    bool dump = argc > 5 && std::string(argv[3]) == "--dump";
    // --cover <frames> <out.png>: plays the opening without input and saves the most colourful frame
    bool cover = argc > 5 && std::string(argv[3]) == "--cover";

    const char *home = getenv("HOME");
    std::string base = std::string(home ? home : ".") + "/Library/Application Support/PopUp16";
    mkdir(base.c_str(), 0755);
    sysDir = base + "/system"; saveDir = base + "/saves";
    std::string profDir = base + "/profiles";
    mkdir(sysDir.c_str(), 0755); mkdir(saveDir.c_str(), 0755); mkdir(profDir.c_str(), 0755);

    std::string name = romPath.substr(romPath.find_last_of('/') + 1);
    std::string stem = name.substr(0, name.find_last_of('.'));
    profilePath = profDir + "/" + stem + ".cfg";
    sramPath = saveDir + "/" + stem + ".srm";
    statePath = saveDir + "/" + stem + ".state";
    loadProfile();

    // ROM: .sfc/.smc directly, or the best dump inside a .zip / .7z (GoodSNES-style sets)
    std::vector<uint8_t> rom;
    std::string ext = lower(name.substr(name.find_last_of('.') + 1));
    if (ext == "zip" || ext == "7z") {
        std::string q = shq(romPath);
        std::string sz = sevenZip();
        std::string listCmd = ext == "7z" ? sz + " l -ba -slt " + q + " | sed -n 's/^Path = //p'" : "/usr/bin/unzip -Z1 " + q;
        if (ext == "7z" && sz.empty()) { fprintf(stderr, "7z support needs: brew install sevenzip\n"); return 1; }
        std::string best; int bestScore = -100000;
        FILE *p = popen(listCmd.c_str(), "r");
        char buf[2048];
        while (p && fgets(buf, sizeof buf, p)) {
            std::string e = buf;
            while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
            int sc = scoreDump(e);
            if (sc > bestScore) { bestScore = sc; best = e; }
        }
        if (p) pclose(p);
        if (best.empty()) { fprintf(stderr, "no .sfc/.smc inside %s\n", romPath.c_str()); return 1; }
        std::string tmp = base + "/tmp";
        mkdir(tmp.c_str(), 0755);
        std::string out = tmp + "/current.rom";
        std::string ex = (ext == "7z" ? sz + " e -so " + q + " " + shq(best) : "/usr/bin/unzip -p " + q + " " + shq(best)) + " > " + shq(out) + " 2>/dev/null";
        if (system(ex.c_str()) != 0) { fprintf(stderr, "extract failed: %s\n", best.c_str()); return 1; }
        printf("picked %s\n", best.c_str());
        romPath = out;
    }
    if (!readFile(romPath, rom)) { fprintf(stderr, "cannot read %s\n", romPath.c_str()); return 1; }

    core.h = dlopen(corePath.c_str(), RTLD_NOW);
    if (!core.h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    sym(core.init, "retro_init"); sym(core.deinit, "retro_deinit");
    sym(core.set_environment, "retro_set_environment");
    sym(core.set_video_refresh, "retro_set_video_refresh");
    sym(core.set_audio_sample, "retro_set_audio_sample");
    sym(core.set_audio_sample_batch, "retro_set_audio_sample_batch");
    sym(core.set_input_poll, "retro_set_input_poll");
    sym(core.set_input_state, "retro_set_input_state");
    sym(core.get_system_av_info, "retro_get_system_av_info");
    sym(core.load_game, "retro_load_game"); sym(core.unload_game, "retro_unload_game");
    sym(core.run, "retro_run");
    sym(core.get_memory_data, "retro_get_memory_data"); sym(core.get_memory_size, "retro_get_memory_size");
    sym(core.serialize_size, "retro_serialize_size");
    sym(core.serialize, "retro_serialize"); sym(core.unserialize, "retro_unserialize");
    core.get_layers = (const uint8_t *(*)(void))dlsym(core.h, "snes3d_get_layers");
    core.get_depths = (const uint8_t *(*)(void))dlsym(core.h, "snes3d_get_depths");
    if (!core.get_layers) fprintf(stderr, "warning: core has no layer export; output will be flat\n");

    core.set_environment(environment);
    core.init();
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);

    retro_game_info gi = {romPath.c_str(), rom.data(), rom.size(), nullptr};
    if (!core.load_game(&gi)) { fprintf(stderr, "core refused ROM\n"); return 1; }
    {
        std::vector<uint8_t> s;
        size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (n && readFile(sramPath, s) && s.size() == n)
            memcpy(core.get_memory_data(RETRO_MEMORY_SAVE_RAM), s.data(), n);
    }
    retro_system_av_info av;
    core.get_system_av_info(&av);

    stereo::makeLut(lut565, false);

    if (cover) {
        int n = atoi(argv[4]);
        std::vector<uint16_t> best;
        unsigned bw = 0, bh = 0;
        double bestScore = -1;
        int bestFrame = 0;
        for (int i = 0; i < n; i++) {
            core.run();
            if (i < 600 || i % 30 || !cur.valid) continue;  // publisher logos fill the first ~10 s
            // score: distinct colours (title art is colourful) weighted by how much of the screen is lit
            static std::vector<uint8_t> seen(4096);
            std::fill(seen.begin(), seen.end(), 0);
            int distinct = 0, lit = 0;
            for (uint16_t c : cur.rgb565) {
                int q = ((c >> 12) << 8) | (((c >> 7) & 15) << 4) | ((c >> 1) & 15);
                if (!seen[q]) { seen[q] = 1; distinct++; }
                int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
                lit += (r + g / 2 + b) > 12;
            }
            double litFrac = (double)lit / cur.rgb565.size();
            // logos sit on one flat colour; title art fills the screen
            static std::vector<int> count(4096);
            std::fill(count.begin(), count.end(), 0);
            int top = 0;
            for (uint16_t c : cur.rgb565) {
                int q = ((c >> 12) << 8) | (((c >> 7) & 15) << 4) | ((c >> 1) & 15);
                top = std::max(top, ++count[q]);
            }
            double flat = (double)top / cur.rgb565.size();
            double score = std::min(distinct, 400) * (0.15 + litFrac) * (1.0 - flat) * (1.0 - flat);
            if (score > bestScore) { bestScore = score; best = cur.rgb565; bw = cur.w; bh = cur.h; bestFrame = i; }
        }
        if (best.empty()) { fprintf(stderr, "no frame\n"); return 1; }
        bool ok = png::write565(argv[5], (int)bw, (int)bh, best.data());
        printf("cover: frame %d score %.0f %ux%u -> %s\n", bestFrame, bestScore, bw, bh, ok ? argv[5] : "WRITE FAILED");
        return ok ? 0 : 1;
    }

    if (dump) {
        char *end = nullptr;
        long frames = strtol(argv[4], &end, 10);
        if (!end || *end || frames < 1 || frames > 100000) {
            fprintf(stderr, "dump frames must be an integer from 1 to 100000\n"); return 1;
        }
        int n = (int)frames;
        bool scripted = getenv("POPUP16_AUTO") != nullptr;
        // POPUP16_PLANES=<prefix>: also write each solo layer (BG1-4, OBJ) as <prefix>N.bmp, empty = magenta
        const char *planePrefix = getenv("POPUP16_PLANES");
        auto enablePlanes = (void (*)(int))dlsym(core.h, "snes3d_enable_planes");
        if (planePrefix && (!enablePlanes || !core.get_layers || !core.get_depths)) {
            fprintf(stderr, "exactness test requires patched layer and plane exports\n"); return 1;
        }
        if (planePrefix) enablePlanes(1);
        if (getenv("POPUP16_REWINDTEST")) {
            // snapshot every 3 frames, then walk the whole history back and compare with the originals
            Rewind rw;
            std::vector<std::vector<uint8_t>> kept;
            size_t sz = core.serialize_size(), raw = 0;
            if (!sz || n < 4) { fprintf(stderr, "rewind test requires state support and at least 4 frames\n"); return 1; }
            double t0 = SDL_GetPerformanceCounter();
            for (int i = 0; i < n; i++) {
                autoFrame = i; core.run();
                if (i % 3 == 0) {
                    std::vector<uint8_t> st(sz);
                    if (!core.serialize(st.data(), sz)) { fprintf(stderr, "rewind snapshot failed at frame %d\n", i); return 1; }
                    rw.push(st); kept.push_back(st); raw += sz;
                }
            }
            double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            size_t depth = rw.depth(), bytes = rw.bytes();
            int bad = 0, steps = 0;
            std::vector<uint8_t> back;
            for (int k = (int)kept.size() - 2; k >= 0 && rw.pop(back); k--, steps++) bad += back != kept[k];
            bool restored = !back.empty() && core.unserialize(back.data(), back.size());
            printf("rewind: %zu snapshots of %zu B, history %zu B (%.1f%% of raw), %d steps back, %d mismatches, restore %s, %.1f ms for %d frames\n",
                   depth + 1, sz, bytes, 100.0 * bytes / std::max<size_t>(raw, 1), steps, bad, restored ? "ok" : "FAILED", ms, n);
            bool complete = steps == (int)kept.size() - 1;
            if (!complete) fprintf(stderr, "rewind history incomplete: %d of %zu steps\n", steps, kept.size() - 1);
            return bad == 0 && restored && complete ? 0 : 1;
        }
        for (int i = 0; i < n; i++) { if (scripted) autoFrame = i; core.run(); }
        if (!cur.valid) { fprintf(stderr, "no frame\n"); return 1; }
        buildSbs();
        int W = 2 * EYE_W, H = (int)outH, rowB = (W * 3 + 3) & ~3;
        std::vector<uint8_t> bmp(54 + (size_t)rowB * H, 0);
        auto put32 = [&](int o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
        bmp[0] = 'B'; bmp[1] = 'M'; put32(2, (uint32_t)bmp.size()); put32(10, 54); put32(14, 40);
        put32(18, W); put32(22, H); bmp[26] = 1; bmp[28] = 24; put32(34, (uint32_t)(rowB * H));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t c = sbs[(size_t)y * W + x];
                uint8_t *px = &bmp[54 + (size_t)(H - 1 - y) * rowB + x * 3];
                px[0] = c & 255; px[1] = (c >> 8) & 255; px[2] = (c >> 16) & 255;
            }
        if (!writeFile(argv[5], bmp.data(), bmp.size())) return 1;
        int verificationStatus = 0;
        if (planePrefix) {
            auto pc = (const uint16_t *(*)(int))dlsym(core.h, "snes3d_get_plane_color");
            auto pz = (const uint8_t *(*)(int))dlsym(core.h, "snes3d_get_plane_z");
            if (!pc || !pz) { fprintf(stderr, "exactness test requires solo plane exports\n"); return 1; }
            unsigned w = cur.w, h = cur.h, ppl = 512;  // GFX.Pitch / 2 (MAX_SNES_WIDTH)
            if (!w || !h || w > diorama::TEX_W || h > diorama::TEX_H) {
                fprintf(stderr, "unsupported exactness frame size %ux%u\n", w, h); return 1;
            }
            int rb = (w * 3 + 3) & ~3;
            for (int n = 0; n < 5; n++) {
                std::vector<uint8_t> pb(54 + (size_t)rb * h, 0);
                auto p32 = [&](int o, uint32_t v) { memcpy(&pb[o], &v, 4); };
                pb[0] = 'B'; pb[1] = 'M'; p32(2, (uint32_t)pb.size()); p32(10, 54); p32(14, 40);
                p32(18, w); p32(22, h); pb[26] = 1; pb[28] = 24; p32(34, (uint32_t)(rb * h));
                const uint16_t *c = pc(n); const uint8_t *z = pz(n);
                if (!c || !z) { fprintf(stderr, "missing solo plane %d\n", n); return 1; }
                int count = 0;
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++) {
                        uint8_t *px = &pb[54 + (size_t)(h - 1 - y) * rb + x * 3];
                        uint32_t v = z[y * ppl + x] ? lut565[c[y * ppl + x]] : 0xffff00ff;
                        count += z[y * ppl + x] != 0;
                        px[0] = v & 255; px[1] = (v >> 8) & 255; px[2] = (v >> 16) & 255;
                    }
                if (!writeFile(std::string(planePrefix) + std::to_string(n) + ".bmp", pb.data(), pb.size())) return 1;
                printf("plane %d: %d px\n", n, count);
            }
            // diorama check: stacking the sheets far to near, seen head-on, must give back the frame
            std::vector<uint16_t> pcol[5]; std::vector<uint8_t> pzz[5];
            diorama::Input din{w, h, cur.rgb565.data(), cur.layers.data(), cur.depths.data(), {}, {}};
            for (int n = 0; n < 5; n++) {
                pcol[n].resize(w * h); pzz[n].resize(w * h);
                for (unsigned y = 0; y < h; y++) {
                    memcpy(&pcol[n][y * w], pc(n) + y * ppl, w * 2);
                    memcpy(&pzz[n][y * w], pz(n) + y * ppl, w);
                }
                din.planeColor[n] = pcol[n].data(); din.planeZ[n] = pzz[n].data();
            }
            auto getM7 = (void (*)(const int16_t **, const uint8_t **, const uint16_t **, int *))dlsym(core.h, "snes3d_get_mode7");
            if (getM7) getM7(&din.m7lines, &din.vram, &din.cgram, &din.m7flags);
            static diorama::Builder db;
            db.look.on = false;  // the exactness check runs on the plain sheets
            db.build(din);
            std::vector<uint32_t> out(w * h, 0);
            for (const auto &q : db.quads)
                for (int x = (int)q.u0; x < (int)q.u1; x++) {
                    uint32_t t = db.slice((int)q.slice)[(int)q.v * diorama::TEX_W + x];
                    if ((t >> 24) == (uint32_t)q.z) out[(int)q.v * w + x] = t & 0xffffff;
                }
            int bad = 0;
            for (size_t i = 0; i < out.size(); i++) bad += out[i] != (db.lut[cur.rgb565[i]] & 0xffffff);
            int sheets = 0; { std::vector<int> seen; for (auto &q : db.quads) { int k = (int)q.slice * 256 + (int)q.z; if (std::find(seen.begin(), seen.end(), k) == seen.end()) seen.push_back(k); } sheets = (int)seen.size(); }
            printf("diorama: %zu quads, %d sheets, head-on mismatch %d of %zu px\n", db.quads.size(), sheets, bad, out.size());
            verificationStatus = bad == 0 ? 0 : 1;
            if (db.frameHasM7) {
                // HD floor check: the map colour at each floor pixel's centre must match what the SNES drew
                int first = din.m7flags >> 9, n7 = 0, ok7 = 0;
                float dmin = 1e9f, dmax = -1e9f;
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++) {
                        size_t i = (size_t)y * w + x;
                        if (cur.layers[i] != 6 || !din.planeZ[0][i]) continue;
                        float u, v;
                        diorama::mode7uv(din.m7lines, din.m7flags, first + (int)y, x + 0.5f, first + y + 0.5f, u, v);
                        uint32_t m = db.map7[((int)floorf(v) & 1023) * 1024 + ((int)floorf(u) & 1023)];
                        uint16_t c = din.planeColor[0][i];
                        int r1 = (c >> 11) & 31, g1 = (c >> 6) & 31, b1 = c & 31;
                        int r2 = (m & 255) >> 3, g2 = ((m >> 8) & 255) >> 3, b2 = ((m >> 16) & 255) >> 3;
                        n7++; ok7 += abs(r1 - r2) <= 2 && abs(g1 - g2) <= 2 && abs(b1 - b2) <= 2;
                    }
                for (auto &q : db.quads) if (q.m7) { dmin = std::min(dmin, q.disparity); dmax = std::max(dmax, q.disparity); }
                printf("mode7: %d floor px, %d match the HD map (%.1f%%), floor disparity %.2f..%.2f\n", n7, ok7, 100.0 * ok7 / std::max(n7, 1), dmin, dmax);
                // preview: 4x frame, floor pixels resampled from the HD map (bilinear across the 4 nearest texels)
                const int S4 = 4, W4 = w * S4, H4 = h * S4, rb4 = (W4 * 3 + 3) & ~3;
                std::vector<uint8_t> pb(54 + (size_t)rb4 * H4, 0);
                auto p32 = [&](int o, uint32_t v) { memcpy(&pb[o], &v, 4); };
                pb[0] = 'B'; pb[1] = 'M'; p32(2, (uint32_t)pb.size()); p32(10, 54); p32(14, 40);
                p32(18, W4); p32(22, H4); pb[26] = 1; pb[28] = 24; p32(34, (uint32_t)(rb4 * H4));
                for (int Y = 0; Y < H4; Y++)
                    for (int X = 0; X < W4; X++) {
                        int x = X / S4, y = Y / S4;
                        size_t i = (size_t)y * w + x;
                        uint32_t c = db.lut[cur.rgb565[i]];
                        if (cur.layers[i] == 6) {
                            float u, v;
                            diorama::mode7uv(din.m7lines, din.m7flags, first + y, (X + 0.5f) / S4, first + (Y + 0.5f) / S4, u, v);
                            int iu = (int)floorf(u) & 1023, iv = (int)floorf(v) & 1023;
                            uint32_t m = db.map7[iv * 1024 + iu];
                            if (m >> 24) c = m & 0xffffff;
                        }
                        uint8_t *px = &pb[54 + (size_t)(H4 - 1 - Y) * rb4 + X * 3];
                        px[0] = (c >> 16) & 255; px[1] = (c >> 8) & 255; px[2] = c & 255;
                    }
                if (!writeFile(std::string(planePrefix) + "hd.bmp", pb.data(), pb.size())) return 1;
            }
            {   // the same view with the pop-up look baked in, written next to the planes
                db.look.on = true;
                {
                    double best = 1e9;
                    for (int rep = 0; rep < 20; rep++) { db.build(din); best = std::min(best, db.profMs[3]); }
                    printf("look bake: %.3f ms (best of 20)\n", best);
                }
                std::vector<uint32_t> lk(w * h, 0);
                for (const auto &q : db.quads)
                    for (int x = (int)q.u0; x < (int)q.u1; x++) {
                        uint32_t t = db.slice((int)q.slice)[(int)q.v * diorama::TEX_W + x];
                        if ((t >> 24) == (uint32_t)q.z) lk[(int)q.v * w + x] = t;
                    }
                std::vector<uint8_t> pb(54 + (size_t)rb * h, 0);
                auto p32 = [&](int o, uint32_t v) { memcpy(&pb[o], &v, 4); };
                pb[0] = 'B'; pb[1] = 'M'; p32(2, (uint32_t)pb.size()); p32(10, 54); p32(14, 40);
                p32(18, w); p32(22, h); pb[26] = 1; pb[28] = 24; p32(34, (uint32_t)(rb * h));
                for (unsigned y = 0; y < h; y++)
                    for (unsigned x = 0; x < w; x++) {
                        uint32_t v = lk[y * w + x];
                        uint8_t *px = &pb[54 + (size_t)(h - 1 - y) * rb + x * 3];
                        px[0] = (v >> 16) & 255; px[1] = (v >> 8) & 255; px[2] = v & 255;  // RGBA bytes -> BGR
                    }
                if (!writeFile(std::string(planePrefix) + "look.bmp", pb.data(), pb.size())) return 1;
            }
            if (getenv("POPUP16_SHEETS"))
                for (auto &q : db.quads) if (q.v == (float)(h / 2)) printf("  row %d slice %d z %d disparity %+.2f span %d-%d\n", (int)q.v, (int)q.slice, (int)q.z, q.disparity, (int)q.u0, (int)q.u1);
        }
        if (getenv("POPUP16_DEBUG")) {
            std::vector<int> pairs(8 * 256, 0);
            for (size_t i = 0; i < cur.layers.size(); i++) pairs[(cur.layers[i] & 7) * 256 + cur.depths[i]]++;
            static const char *nm[8] = {"BG1", "BG2", "BG3", "BG4", "OBJ", "backdrop", "M7", "M7ext"};
            for (int l = 0; l < 8; l++)
                for (int z = 0; z < 256; z++)
                    if (pairs[l * 256 + z] > 100)
                        printf("  %-8s z=%3d px=%6d disparity=%+.1f\n", nm[l], z, pairs[l * 256 + z],
                               l == 6 ? NAN : stereo::baseDisparity(l, z));
        }
        int hist[8] = {0};
        for (uint8_t l : cur.layers) hist[l & 7]++;
        printf("frame %ux%u layers: BG1 %d BG2 %d BG3 %d BG4 %d OBJ %d backdrop %d M7 %d M7ext %d\n",
               cur.w, cur.h, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
        return verificationStatus;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    Uint32 wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (startWindowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Window *win = SDL_CreateWindow("PopUp16", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900, wflags);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SDL_RaiseWindow(win);
    if (!startWindowed) SDL_ShowCursor(SDL_DISABLE);
    keys = SDL_GetKeyboardState(nullptr);
    openPads();

    SDL_AudioSpec want = {}, have = {};
    want.freq = (int)lrint(av.timing.sample_rate);
    want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
    const Uint32 bytesPerFrame = (Uint32)(av.timing.sample_rate / av.timing.fps) * 4;
    const Uint32 targetQueue = bytesPerFrame * 3;

    SDL_Texture *tex = nullptr;
    unsigned texH = 0;
    updateTitle(win, stem);
    printf("PopUp16 running %s\n"
           "  pad/keys: arrows, Z=B X=A A=Y S=X Q=L W=R Enter=Start RShift=Select\n"
           "  1/2 depth -/+   3/4 convergence -/+   0 3D on/off   B half/full SBS   P swap eyes\n"
           "  M Mode 7 ramp   [ ] aspect   F fullscreen   F5 save state   F7 load state   Tab fast-forward   Esc quit\n",
           stem.c_str());

    bool running = true, ff = false;
    Uint64 lastFrame = SDL_GetPerformanceCounter();
    const double frameSec = 1.0 / av.timing.fps;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) openPads();
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                const char *msg = nullptr;
                bool changed = true;
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: running = false; changed = false; break;
                case SDLK_1: prof.strength = std::max(0.0f, prof.strength - 0.25f); break;
                case SDLK_2: prof.strength = std::min(4.0f, prof.strength + 0.25f); break;
                case SDLK_3: prof.convergence -= 0.5f; break;
                case SDLK_4: prof.convergence += 0.5f; break;
                case SDLK_0: stereoOn = !stereoOn; break;
                case SDLK_b: prof.halfSbs ^= 1; break;
                case SDLK_p: prof.swapEyes ^= 1; break;
                case SDLK_m: prof.mode7Ramp ^= 1; break;
                case SDLK_LEFTBRACKET: prof.aspect = std::max(1.0f, prof.aspect - 0.05f); break;
                case SDLK_RIGHTBRACKET: prof.aspect = std::min(2.4f, prof.aspect + 0.05f); break;
                case SDLK_f: {
                    bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    SDL_ShowCursor(fs ? SDL_ENABLE : SDL_DISABLE);
                    changed = false; break;
                }
                case SDLK_F5: {
                    std::vector<uint8_t> s(core.serialize_size());
                    if (core.serialize(s.data(), s.size())) { writeFile(statePath, s.data(), s.size()); msg = "state saved"; }
                    changed = false; break;
                }
                case SDLK_F7: {
                    std::vector<uint8_t> s;
                    msg = (readFile(statePath, s) && core.unserialize(s.data(), s.size())) ? "state loaded" : "no state";
                    changed = false; break;
                }
                default: changed = false;
                }
                if (changed) saveProfile();
                updateTitle(win, stem, msg);
            }
        }
        ff = keys[SDL_SCANCODE_TAB];

        // pace emulation by audio queue depth (works on 60/120 Hz displays alike)
        int runs = 0;
        if (ff) { for (int i = 0; i < 4; i++) core.run(); runs = 4; SDL_ClearQueuedAudio(audioDev); }
        else if (audioDev) {
            while (SDL_GetQueuedAudioSize(audioDev) < targetQueue && runs < 3) { core.run(); runs++; }
        } else {
            Uint64 now = SDL_GetPerformanceCounter();
            if ((double)(now - lastFrame) / SDL_GetPerformanceFrequency() >= frameSec) { core.run(); runs = 1; lastFrame = now; }
        }
        static Uint64 statT = SDL_GetTicks64(); static int statRuns = 0, statPresents = 0;
        statRuns += runs; statPresents++;
        if (getenv("POPUP16_DEBUG") && SDL_GetTicks64() - statT >= 2000) {
            fprintf(stderr, "emu %.1f fps, present %.1f fps, audio queue %u B\n", statRuns / 2.0, statPresents / 2.0,
                    audioDev ? SDL_GetQueuedAudioSize(audioDev) : 0);
            statT = SDL_GetTicks64(); statRuns = statPresents = 0;
        }
        if (!runs) { SDL_Delay(1); if (!cur.valid) continue; }

        if (cur.valid && runs) {
            buildSbs();
            if (!tex || texH != outH) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 2 * EYE_W, outH);
                texH = outH;
            }
            SDL_UpdateTexture(tex, nullptr, sbs.data(), 2 * EYE_W * 4);
        }

        int ow, oh;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {
            // each half of the window is one eye; image keeps prof.aspect after VD un-squeezes it
            int halfW = ow / 2;
            double eyeAspectScale = prof.halfSbs ? 0.5 : 1.0;
            int h = oh, w = (int)lrint(h * prof.aspect * eyeAspectScale);
            if (w > halfW) { w = halfW; h = (int)lrint(w / (prof.aspect * eyeAspectScale)); }
            for (int eye = 0; eye < 2; eye++) {
                SDL_Rect srcR = {eye * EYE_W, 0, EYE_W, (int)outH};
                SDL_Rect dstR = {eye * halfW + (halfW - w) / 2, (oh - h) / 2, w, h};
                SDL_RenderCopy(ren, tex, &srcR, &dstR);
            }
        }
        SDL_RenderPresent(ren);
    }

    {
        size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
        if (n) writeFile(sramPath, core.get_memory_data(RETRO_MEMORY_SAVE_RAM), n);
    }
    saveProfile();
    core.unload_game();
    core.deinit();
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
