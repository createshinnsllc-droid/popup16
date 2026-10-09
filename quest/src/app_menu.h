// PopUp16 Quest app: menu.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- menu
static const int MENU_W = 1024, MENU_H = 768, CELL_W = 16, CELL_H = 32;
static const int COLS = MENU_W / CELL_W, ROWS = MENU_H / CELL_H;
static std::vector<uint32_t> menuPixels(MENU_W * MENU_H);
// laser pointer on the menu panel, in menu-image pixels (set each frame by the pointer code)
static struct { bool hover = false, click = false; int px = 0, py = 0; float lx = 0, ly = 0; } ptr;
static int panelH = 768;  // used height of the menu image (short pages get a short panel)
static int libRow0 = 0;                      // first visible library row (for pointer hits)
static int tabX0[3] = {0}, tabX1[3] = {0};   // library tab hit ranges in pixels
enum MenuMode { MENU_NONE, MENU_ROMS, MENU_PAUSE, MENU_HELP, MENU_ABOUT, MENU_ARRANGE, MENU_SLOTS, MENU_CONTROLS };
static int controlsSel = 0;  // rows: actions, then scope, reset, done
static int slotSel = 0;
static bool slotSaving = true;
static std::vector<std::string> aboutLines;
static int aboutTop = 0;
static MenuMode menuMode = MENU_ROMS;
enum LibTab { TAB_RECENT, TAB_FAV, TAB_ALL, TAB_N };
static int libTab = TAB_ALL;
static std::vector<std::string> libView;  // ROMs in the current tab
static bool gripHeldL = true, gripHeldR = true, favHeld = true;
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

static void buildLibView() {
    libView.clear();
    for (auto &r : roms) {
        auto it = stats.find(r);
        bool played = it != stats.end() && it->second.lastPlayed;
        bool fav = it != stats.end() && it->second.fav;
        if (libTab == TAB_ALL || (libTab == TAB_RECENT && played) || (libTab == TAB_FAV && fav)) libView.push_back(r);
    }
    if (libTab == TAB_RECENT)
        std::stable_sort(libView.begin(), libView.end(), [](const std::string &a, const std::string &b) { return stats[a].lastPlayed > stats[b].lastPlayed; });
    romSel = std::clamp(romSel, 0, std::max(0, (int)libView.size() - 1));
}

// small text: the 8x16 font at 1x, placed in pixels
static void drawSmall(int px, int py, const std::string &s, uint32_t color) {
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) c = '?';
        const unsigned char *g = kFont8x16[c - 32];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++) {
                int X = px + (int)i * 8 + x, Y = py + y;
                if (X < 0 || Y < 0 || X >= MENU_W || Y >= MENU_H) continue;
                if (g[y] & (0x80 >> x)) menuPixels[Y * MENU_W + X] = color;
            }
    }
}

static void fillRound(int x0, int y0, int w, int h, int r, uint32_t c) {  // rounded rectangle
    for (int y = std::max(0, y0); y < std::min(MENU_H, y0 + h); y++)
        for (int x = std::max(0, x0); x < std::min(MENU_W, x0 + w); x++) {
            int dx = x < x0 + r ? x0 + r - x : x >= x0 + w - r ? x - (x0 + w - r - 1) : 0;
            int dy = y < y0 + r ? y0 + r - y : y >= y0 + h - r ? y - (y0 + h - r - 1) : 0;
            if (dx * dx + dy * dy <= r * r) menuPixels[y * MENU_W + x] = c;
        }
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
static uint32_t C_BG = 0xff201812, C_TEXT = 0xffe0e0e0, C_DIM = 0xff909090, C_HI = 0xff30c0ff, C_SELBG = 0xff604020;
// normal is the original look; "Menu contrast: high" swaps in brighter text and a stronger selection
static void applyPalette() {
    if (on(cfg.contrast)) {
        C_BG = 0xff0c0a08; C_TEXT = 0xffffffff; C_DIM = 0xffc8c8c8; C_HI = 0xff7fd8ff; C_SELBG = 0xff2f6fb0;
    } else {
        C_BG = 0xff201812; C_TEXT = 0xffe0e0e0; C_DIM = 0xff909090; C_HI = 0xff30c0ff; C_SELBG = 0xff604020;
    }
}


enum PauseItem { P_RESUME, P_CONTROLS, P_SPEED, P_DEPTH, P_CONV, P_3D, P_MODE7, P_SWAP, P_ROOM, P_STYLE, P_LOOK, P_CONTRAST, P_SKY,
                 P_TABLE, P_RESET, P_SIZE, P_DIST, P_SAVE, P_LOAD, P_SHOT, P_CLIP, P_GAMES, P_ABOUT,
                 P_PAGE_PICTURE, P_PAGE_ROOM, P_BACK, P_SEAT, P_HANG, PAUSE_N };
static const char *pauseItems[PAUSE_N] = {
    "Resume", "Controls & remapping", "Game speed", "3D depth", "Convergence", "3D on/off", "Mode 7 floor depth", "Swap eyes",
    "Surroundings", "3D style", "Pop-up look", "Menu contrast", "Show sky", "Tabletop mode", "Bring it in front of me",
    "Screen size", "Screen distance", "Save state", "Load state", "Take screenshot", "Save last 30 s as video",
    "Choose game", "About & licenses", "Picture & 3D  >", "Room & placement  >", "<  Back",
    "This is my seat", "Hang it on the wall"};
// the pause menu is three short pages instead of one long list
static const std::vector<int> pausePages[3] = {
    {P_RESUME, P_GAMES, P_SAVE, P_LOAD, P_SHOT, P_CLIP, P_PAGE_PICTURE, P_PAGE_ROOM, P_CONTROLS, P_ABOUT},
    {P_BACK, P_DEPTH, P_CONV, P_3D, P_LOOK, P_CONTRAST, P_MODE7, P_SWAP, P_SPEED},
    {P_BACK, P_HANG, P_SEAT, P_TABLE, P_ROOM, P_SKY, P_STYLE, P_SIZE, P_DIST, P_RESET}};
static const char *pageTitles[3] = {nullptr, "Picture & 3D", "Room & placement"};
static int pausePage = 0;
static bool adjustable(int i) { return i == P_SPEED || i == P_DEPTH || i == P_CONV || i == P_SIZE || i == P_DIST; }
static bool toggle(int i) { return i == P_3D || i == P_MODE7 || i == P_SWAP || i == P_ROOM || i == P_STYLE || i == P_LOOK || i == P_CONTRAST || i == P_SKY || i == P_TABLE; }
// where the head is (LOCAL space), updated every frame; menus and placement presets follow its yaw
static float headX = 0, headY = 0, headZ = 0, headYaw = 0;
static void yawFwd(float &fx, float &fz) { fx = -sinf(headYaw); fz = -cosf(headYaw); }
static void placeMenu();
// How far the game sits from the viewer it was placed for. Before the seat is marked this is the
// distance from the LOCAL origin, which is exactly the previous behaviour.
static float placementDepth() {
    const place::Vec3 seat = on(cfg.seatMarked) ? place::Vec3{cfg.seatX, cfg.seatY, cfg.seatZ} : place::Vec3{0, 0, 0};
    return place::seatDepth(place::Vec3{cfg.px, cfg.py, cfg.pz}, seat, 0.4f);
}
// Remember where the viewer actually is. Called when the screen is placed or re-placed, and from
// the menu, so window depth follows the person instead of the room they first launched the app in.
static void markSeatHere() {
    cfg.seatX = headX; cfg.seatY = headY; cfg.seatZ = headZ; cfg.seatMarked = 1;
}
static void markSeatAndTell() {
    markSeatHere();
    saveGlobal();
    showToast("Seat marked: picture depth is measured from here now");
    trace("seat marked at %.2f %.2f %.2f", cfg.seatX, cfg.seatY, cfg.seatZ);
}
static void defaultPlacement() {  // straight ahead of where you are looking, upright
    float fx, fz; yawFwd(fx, fz);
    cfg.px = headX + fx * cfg.distance; cfg.py = headY; cfg.pz = headZ + fz * cfg.distance;
    cfg.qx = 0; cfg.qy = sinf(headYaw / 2); cfg.qz = 0; cfg.qw = cosf(headYaw / 2);
}
// Tabletop: a small pop-up box standing on the table in front of you, room visible around it
static bool hasRoomView();
static void setTabletop(bool want) {
    float fx, fz; yawFwd(fx, fz);
    if (want) {
        cfg.table = 1; cfg.box = 1; cfg.sky = 0; cfg.popLook = 1;
        if (hasRoomView()) cfg.room = 1;
        cfg.screenWidth = 0.7f;
        markSeatHere();  // the tabletop is sized for the seat you are in right now
        cfg.px = headX + fx * 0.65f; cfg.py = headY - 0.40f; cfg.pz = headZ + fz * 0.65f;
        // face you, leaning back a little like a book propped open
        float yq = sinf(headYaw / 2), yw = cosf(headYaw / 2), t = -0.26f / 2;  // -15 degrees about x
        float tx = sinf(t), tw = cosf(t);
        cfg.qx = yw * tx; cfg.qy = yq * tw; cfg.qz = -yq * tx; cfg.qw = yw * tw;
        cfg.distance = 0.75f;
    } else {
        cfg.table = 0; cfg.box = 0; cfg.room = 0; cfg.sky = 1;
        cfg.screenWidth = 2.4f; cfg.distance = 2.2f;
        defaultPlacement();
    }
}

static void showToast(const std::string &t);
// ---- save slots: state + a thumbnail of the frame + the time it was made
static std::string slotPath(int n) { return saveDir + "/" + stem() + (n == 0 ? ".state" : ".slot" + std::to_string(n + 1) + ".state"); }
static void saveSlot(int n) {
    std::vector<uint8_t> st(retro_serialize_size());
    if (st.empty() || !retro_serialize(st.data(), st.size())) { showToast("Could not save: the core refused"); return; }
    if (!writeFileAtomic(slotPath(n), st.data(), st.size())) { showToast("Could not write slot " + std::to_string(n + 1)); return; }
    if (frame.valid) {  // the picture is a convenience: its failure must not hide a good save
        std::vector<uint8_t> th(4 + frame.rgb565.size() * 2);
        uint16_t wh[2] = {(uint16_t)frame.w, (uint16_t)frame.h};
        memcpy(th.data(), wh, 4); memcpy(th.data() + 4, frame.rgb565.data(), frame.rgb565.size() * 2);
        if (!writeFileAtomic(slotPath(n) + ".thumb", th.data(), th.size())) LOGE("slot %d thumbnail failed", n);
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
            if (wh[0] >= 16 && wh[1] >= 16 && wh[0] <= 1024 && wh[1] <= 1024 && th.size() >= 4 + (size_t)wh[0] * wh[1] * 2) {
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
    // short pause pages get a panel just tall enough for them, instead of a mostly empty board
    panelH = MENU_H;
    if (menuMode == MENU_PAUSE) {
        int n = (int)pausePages[pausePage].size();
        panelH = std::min(MENU_H, (n + 2) * CELL_H + (pausePage == 2 ? 56 : 0) + 64);
    }
    std::fill(menuPixels.begin(), menuPixels.end(), 0u);         // transparent outside the panel
    fillRound(0, 0, MENU_W, panelH, 28, C_BG);
    for (int x = 24; x < MENU_W - 24; x++) menuPixels[(CELL_H + 6) * MENU_W + x] = 0xff3a3028;  // header rule
    if (menuMode == MENU_NONE) {  // playing: only a notice strip (see the small quad in the frame loop)
        if (!toast.empty()) drawText(1, 1, toast.substr(0, COLS - 2), C_HI);
        menuDirty = false;
        return;
    }
    char buf[160];
    if (menuMode == MENU_ROMS) {
        static const char *tabs[TAB_N] = {"Recent", "Favorites", "All games"};
        drawText(1, 0, "PopUp16", C_HI);
        int tx = 12;
        for (int t = 0; t < TAB_N; t++) {
            std::string label = std::string(" ") + tabs[t] + " ";
            tabX0[t] = tx * CELL_W; tabX1[t] = (tx + (int)label.size()) * CELL_W;
            if (t == libTab) fillRound(tx * CELL_W, 2, (int)label.size() * CELL_W, CELL_H - 4, 10, C_SELBG);
            drawText(tx, 0, label, t == libTab ? 0xffffffff : C_DIM);
            tx += (int)label.size() + 1;
        }
        if (roms.empty()) {
            drawText(1, 3, "No games found. Copy .sfc files to:", C_TEXT);
            drawText(1, 4, romDir.substr(0, COLS - 2), C_DIM);
        } else if (libView.empty()) {
            drawText(1, 3, libTab == TAB_FAV ? "No favorites yet: press left X on a game to star it."
                                             : "Nothing played yet. Pick something from All games.", C_TEXT);
        } else {
            const int cols = 4, cardW = 248, cardH = 336, top = 40;
            int row0 = std::max(0, romSel / cols - 1);
            row0 = std::min(row0, std::max(0, ((int)libView.size() - 1) / cols - 1));
            libRow0 = row0;
            for (int i = 0; i < 2 * cols; i++) {
                int idx = row0 * cols + i;
                if (idx >= (int)libView.size()) break;
                int x0 = 16 + (i % cols) * cardW, y0 = top + (i / cols) * cardH;
                const std::string &rom = libView[idx];
                bool sel = idx == romSel;
                if (sel)
                    for (int y = y0; y < y0 + cardH - 8; y++)
                        for (int x = x0; x < x0 + cardW - 8; x++) menuPixels[y * MENU_W + x] = (y < y0 + 4 || y >= y0 + cardH - 12 || x < x0 + 4 || x >= x0 + cardW - 12) ? C_HI : C_SELBG;
                const std::vector<uint32_t> *cv = cover(rom);
                int cx = x0 + 8, cy = y0 + 8;
                for (int y = 0; y < CARD_H; y++)
                    for (int x = 0; x < CARD_W; x++)
                        menuPixels[(cy + y) * MENU_W + cx + x] = cv ? (*cv)[y * CARD_W + x] : 0xff3a2a20;
                std::string name = rom.substr(0, rom.find_last_of('.'));
                if (!cv) drawSmall(cx + 8, cy + CARD_H / 2 - 8, name.substr(0, 26), C_DIM);
                // title on up to two lines, then playtime and last played
                std::string l1 = name, l2;
                if (name.size() > 28) {  // wrap at the last space that fits
                    size_t cut = name.rfind(' ', 28);
                    if (cut == std::string::npos || cut < 12) cut = 28;
                    l1 = name.substr(0, cut);
                    l2 = name.substr(cut + (name[cut] == ' ' ? 1 : 0));
                    if (l2.size() > 28) l2 = l2.substr(0, 26) + "..";
                }
                drawSmall(cx, cy + CARD_H + 8, l1, sel ? 0xffffffff : C_TEXT);
                if (!l2.empty()) drawSmall(cx, cy + CARD_H + 26, l2, sel ? 0xffffffff : C_TEXT);
                auto it = stats.find(rom);
                std::string meta = it != stats.end() ? playtimeText(it->second.seconds) : "new";
                std::string lp = it != stats.end() ? lastPlayedText(it->second.lastPlayed) : "";
                if (!lp.empty()) meta += "  -  " + lp;
                drawSmall(cx, cy + CARD_H + 50, meta, C_DIM);
                if (it != stats.end() && it->second.fav) drawSmall(cx + CARD_W - 16, cy + CARD_H + 50, "*", 0xff30d0ff);
            }
        }
        snprintf(buf, sizeof buf, "%d/%d  A: play  X: favorite  grips: tabs  B: back", libView.empty() ? 0 : romSel + 1, (int)libView.size());
        drawText(1, ROWS - 1, buf, C_DIM);
    } else if (menuMode == MENU_PAUSE) {
        const std::vector<int> &items = pausePages[pausePage];
        std::string title = pageTitles[pausePage] ? pageTitles[pausePage] : stem();
        if ((int)title.size() > COLS - 4) title = title.substr(0, COLS - 4);
        drawText(1, 0, title, C_HI);
        for (int k = 0; k < (int)items.size(); k++) {
            int i = items[k];
            std::string v;
            switch (i) {
            case P_SPEED: snprintf(buf, sizeof buf, "%.2gx%s", cfg.speed, cfg.speed < 1 ? " slow-mo" : cfg.speed > 1 ? " fast" : ""); v = buf; break;
            case P_DEPTH: snprintf(buf, sizeof buf, "%.2f", cfg.strength); v = buf; break;
            case P_CONV: snprintf(buf, sizeof buf, "%+.1f", cfg.convergence); v = buf; break;
            case P_3D: v = on(cfg.stereoOn) ? "on" : "off"; break;
            case P_MODE7: v = on(cfg.mode7Ramp) ? "on" : "off"; break;
            case P_SWAP: v = on(cfg.swapEyes) ? "swapped" : "normal"; break;
            case P_ROOM: v = on(cfg.room) ? "your room" : "dark void"; break;
            case P_STYLE: v = cfg.box > 1.5f ? "window in the wall" : on(cfg.box) ? "pop-up box" : "big screen"; break;
            case P_LOOK: v = on(cfg.popLook) ? "shadows + edges" : "classic"; break;
            case P_CONTRAST: v = on(cfg.contrast) ? "high" : "normal"; break;
            case P_SKY: v = on(cfg.sky) ? "on" : "off"; break;
            case P_TABLE: v = on(cfg.table) ? "on" : "off"; break;
            case P_SIZE: snprintf(buf, sizeof buf, "%.1f m", cfg.screenWidth); v = buf; break;
            case P_DIST: snprintf(buf, sizeof buf, "%.1f m", cfg.distance); v = buf; break;
            case P_SEAT: v = on(cfg.seatMarked) ? "depth from your seat" : "not marked yet"; break;
            }
            bool sel = k == pauseSel;
            int y = (k + 2) * CELL_H;
            if (sel) fillRound(16, y + 1, MENU_W - 32, CELL_H - 2, 10, C_SELBG);
            drawText(2, k + 2, pauseItems[i], sel ? 0xffffffff : C_TEXT);
            bool arrows = adjustable(i) || toggle(i);
            if (!v.empty()) drawText(30, k + 2, (arrows ? "< " : "") + v + (arrows ? " >" : ""), sel ? 0xffffffff : C_HI);
        }
        if (pausePage == 2) {
            drawSmall(32, (int)(items.size() + 3) * CELL_H, "Move the game any time: point at the bar under it and hold the trigger.", C_TEXT);
            drawSmall(32, (int)(items.size() + 3) * CELL_H + 20, "While holding, push the thumbstick up or down to resize it.", C_TEXT);
        }
        drawSmall(16, panelH - 28, "Point and pull the trigger, or use the sticks and A.   B: back   Left Y: close", C_DIM);
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
    else if (menuMode == MENU_CONTROLS) {
        drawText(1, 0, "Controls & remapping", C_HI);
        int rows = N_ACTIONS + 3;
        for (int r = 0; r < rows; r++) {
            int y = r + 2 + (r >= N_ACTIONS ? 1 : 0);
            bool sel = r == controlsSel;
            if (sel) for (int x = 0; x < MENU_W; x++) for (int yy = 0; yy < CELL_H; yy++) menuPixels[(y * CELL_H + yy) * MENU_W + x] = C_SELBG;
            uint32_t col = sel ? 0xffffffff : C_TEXT;
            if (r < N_ACTIONS) {
                int a = kActions[r];
                drawText(2, y, actionName(a), col);
                std::string v = (remapAction == a) ? "press a button...  (left Y cancels)" : boundTo(a);
                drawText(24, y, v.substr(0, COLS - 25), remapAction == a ? C_HI : col);
            } else if (r == N_ACTIONS) {
                drawText(2, y, "Applies to", col);
                drawText(24, y, controlsPerGame ? "< this game only >" : "< all games >", col);
            } else if (r == N_ACTIONS + 1) {
                drawText(2, y, "Reset to defaults", col);
            } else {
                drawText(2, y, "Done", col);
            }
        }
        drawText(1, ROWS - 2, "A: assign a button   X: clear   menus always use A / B", C_DIM);
        drawText(1, ROWS - 1, "Left Y opens the PopUp16 menu and cannot be reassigned.", C_DIM);
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
        std::vector<std::string> help = controlsHelp();
        for (int i = 0; i < (int)help.size() && i < ROWS - 1; i++)
            drawText(1, i + 1, help[i].substr(0, COLS - 2), i == 0 ? C_HI : C_TEXT);
    }
    if (!toast.empty() && nowSec() < toastUntil) drawText(1, ROWS - 2, toast, C_HI);
    menuDirty = false;
}
static void showToast(const std::string &t) { toast = t; toastUntil = nowSec() + 2.0; menuDirty = true; }
