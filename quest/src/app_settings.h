// PopUp16 Quest app: settings.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- settings
struct Settings {
    // per game
    float strength = 1.0f, convergence = 0.0f, mode7Ramp = 1, swapEyes = 0, stereoOn = 1;
    // global: how and where the diorama sits in the room
    float screenWidth = 2.4f, distance = 2.2f, room = 0, box = 0, popLook = 1, sky = 1, speed = 1, table = 0, contrast = 0;
    float px = 0, py = 0, pz = -2.2f, qx = 0, qy = 0, qz = 0, qw = 1;
    // the viewer position the screen was placed for; window/box depth is measured from here rather
    // than from the LOCAL origin, so a recenter or a different chair cannot leave it stale
    float seatX = 0, seatY = 0, seatZ = 0, seatMarked = 0;
    // global: what the app opens to at launch (0 = continue the last game, 1 = the library)
    float startLibrary = 0;
};
static Settings cfg;
static std::string filesDir, romDir, saveDir, sysDir;
static const struct { const char *name; float Settings::*field; bool global; float lo, hi; } kFields[] = {
    {"strength", &Settings::strength, false, 0.0f, 3.0f}, {"convergence", &Settings::convergence, false, -20.0f, 20.0f},
    {"mode7Ramp", &Settings::mode7Ramp, false, 0.0f, 1.0f}, {"swapEyes", &Settings::swapEyes, false, 0.0f, 1.0f},
    {"stereoOn", &Settings::stereoOn, false, 0.0f, 1.0f}, {"screenWidth", &Settings::screenWidth, true, 0.3f, 8.0f},
    {"distance", &Settings::distance, true, 0.4f, 8.0f}, {"room", &Settings::room, true, 0.0f, 1.0f}, {"box", &Settings::box, true, 0.0f, 2.0f},
    {"popLook", &Settings::popLook, true, 0.0f, 1.0f}, {"sky", &Settings::sky, true, 0.0f, 1.0f}, {"speed", &Settings::speed, true, 0.25f, 4.0f}, {"table", &Settings::table, true, 0.0f, 1.0f},
    {"contrast", &Settings::contrast, true, 0.0f, 1.0f}, {"startLibrary", &Settings::startLibrary, true, 0.0f, 1.0f},
    {"px", &Settings::px, true, -50.0f, 50.0f}, {"py", &Settings::py, true, -50.0f, 50.0f}, {"pz", &Settings::pz, true, -50.0f, 50.0f},
    {"qx", &Settings::qx, true, -1.0f, 1.0f}, {"qy", &Settings::qy, true, -1.0f, 1.0f}, {"qz", &Settings::qz, true, -1.0f, 1.0f}, {"qw", &Settings::qw, true, -1.0f, 1.0f},
    {"seatX", &Settings::seatX, true, -50.0f, 50.0f}, {"seatY", &Settings::seatY, true, -50.0f, 50.0f}, {"seatZ", &Settings::seatZ, true, -50.0f, 50.0f},
    {"seatMarked", &Settings::seatMarked, true, 0.0f, 1.0f},
};
static bool on(float v) { return v > 0.5f; }
static void flip(float &v) { v = on(v) ? 0.0f : 1.0f; }

// defined below; declared here because settings are written before it appears
static bool writeFileAtomic(const std::string &p, const void *d, size_t n);

static bool saveSettings(const std::string &path, bool global) {
    std::string text;
    char line[96];
    for (auto &k : kFields) if (k.global == global) { snprintf(line, sizeof line, "%s=%g\n", k.name, cfg.*k.field); text += line; }
    if (text.empty()) return true;                  // nothing belongs in this file; leave any existing one alone
    return writeFileAtomic(path, text.data(), text.size());
}
static void loadSettings(const std::string &path, bool global) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char k[64]; float v;
    while (fscanf(f, "%63[^=]=%f\n", k, &v) == 2)
        for (auto &e : kFields)
            if (e.global == global && !strcmp(e.name, k) && std::isfinite(v) && v >= e.lo && v <= e.hi) cfg.*e.field = v;
    fclose(f);
}
static void saveGlobal() { saveSettings(filesDir + "/settings.cfg", true); }

// per-game library stats (see the library section)
struct GameStats { double seconds = 0; long lastPlayed = 0; bool fav = false; };
static std::map<std::string, GameStats> stats;  // by ROM file name
static std::string coverDir;
static bool saveStats();
static void showToast(const std::string &t);

static bool readFile(const std::string &p, std::vector<uint8_t> &out) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}
static bool writeFile(const std::string &p, const void *d, size_t n) {
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}
// Crash-safe, checked write for anything the user would hate to lose: the data goes to a temporary
// file beside the destination, is flushed all the way to disk, and only then replaces the old file.
// A failure (full disk, bad permissions, interrupted write) leaves the previous good file intact and
// reports false, so callers can tell the user the truth instead of claiming success.
static bool writeFileAtomic(const std::string &p, const void *d, size_t n) {
    if (n == 0) return false;                       // never replace a good file with nothing
    std::string tmp = p + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { LOGE("cannot open %s for writing", tmp.c_str()); return false; }
    bool ok = fwrite(d, 1, n, f) == n;
    if (ok && fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp.c_str()); LOGE("write failed for %s", p.c_str()); return false; }
    if (rename(tmp.c_str(), p.c_str()) != 0) { remove(tmp.c_str()); LOGE("replacing %s failed", p.c_str()); return false; }
    return true;
}
