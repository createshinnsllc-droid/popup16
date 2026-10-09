// PopUp16 Quest app: core.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

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
        float thickness = 0.0f;    // sprite card thickness in SNES pixels (0 = flat)
        unsigned gen = 0;          // game generation this frame belongs to
    };
    Job job;                      // filled by the display loop under lock
    bool hasJob = false, quit = false;
    diorama::Builder builders[3];     // three, so building never touches the one waiting or uploading
    int ready = -1, uploading = -1;   // finished build waiting for upload; build being uploaded
    unsigned builtGen[3] = {0, 0, 0}; // game generation each builder last built
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
            b.mode7Ramp = local.ramp; b.look.on = local.look; b.showBackdrop = local.backdrop; b.thickness = local.thickness;
            b.build(din);
            std::lock_guard<std::mutex> l(m);
            builtGen[target] = local.gen;
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
    if (!muteAudio) recorder.pushFrame([&] {  // tightly packed copy for the clip history
        static std::vector<uint16_t> t;
        t.resize(w * h);
        for (unsigned y = 0; y < h; y++) memcpy(&t[y * w], (const uint8_t *)data + y * pitch, w * 2);
        return t.data();
    }(), w, h);
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
static std::string picturesDir, moviesDir;
static double playedThisLoad = 0;  // seconds of play since the game was loaded (cover capture)
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
static std::string controlsPath(bool perGame) {
    return perGame && gameLoaded ? saveDir + "/" + stem() + ".controls" : filesDir + "/controls.cfg";
}
static void saveSram() {
    if (!gameLoaded) return;
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    // In-game progress: a failed write has to be visible, not silently swallowed.
    if (n && !writeFileAtomic(saveDir + "/" + stem() + ".srm", retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), n)) {
        LOGE("battery save failed for %s", stem().c_str());
        showToast("Could not save game progress");
    }
}
// quick resume: the running game's state and name are saved whenever the app may go away
static void saveResume() {
    if (!gameLoaded) return;
    saveStats();
    std::vector<uint8_t> s(retro_serialize_size());
    if (s.empty() || !retro_serialize(s.data(), s.size())) { LOGE("could not serialize %s for resume", gameName.c_str()); return; }
    // Point last_game.txt at this game only after its state is safely on disk, so a failed save can
    // never send the next launch into a missing or stale snapshot.
    if (writeFileAtomic(saveDir + "/" + stem() + ".resume", s.data(), s.size()))
        writeFileAtomic(filesDir + "/last_game.txt", gameName.data(), gameName.size());
    else
        LOGE("resume save failed for %s", stem().c_str());
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
static unsigned gameGen = 0;  // bumped on every game load; tags frames handed to the build thread
static bool loadGame(const std::string &name) {
    // only plain file names from the ROM folder (never a path that could leave it)
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") return false;
    unloadGame();
    gameGen++;
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
    recorder.clear();
    gameLoaded = true; gameName = name; gamePath = path;
    retro_system_av_info av; retro_get_system_av_info(&av);
    avFps = av.timing.fps; avRate = av.timing.sample_rate;
    recorder.fps = avFps;
    resampleStep = avRate / OUT_RATE;
    std::vector<uint8_t> s;
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (n && readFile(saveDir + "/" + stem() + ".srm", s) && s.size() == n) memcpy(retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), s.data(), n);
    {   // per-game values start from the defaults, then this game's own file (if any)
        Settings d;
        for (auto &e : kFields) if (!e.global) cfg.*e.field = d.*e.field;
        loadSettings(saveDir + "/" + stem() + ".cfg", false);
    }
    {
        struct stat sb;
        controlsPerGame = stat(controlsPath(true).c_str(), &sb) == 0;
        loadMapping(controlsPath(controlsPerGame));
    }
    trace("loaded %s (%.3f fps, %.1f Hz)", name.c_str(), avFps, avRate);
    stats[name].lastPlayed = (long)time(nullptr);
    saveStats();
    playedThisLoad = 0;
    return true;
}
