// PopUp16 Quest app: menu input.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

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
    bool okBtn = b[B_B];
    if (menuMode == MENU_ARRANGE) {  // triggers are busy grabbing; only A / B leave
        bool done = (okBtn && !okHeld) || (b[B_A] && !backHeld);
        okHeld = okBtn; backHeld = b[B_A];
        if (done) { menuMode = MENU_PAUSE; saveGlobal(); menuDirty = true; }
        return;
    }
    bool ok = okBtn && !okHeld;
    okHeld = okBtn;
    bool pclick = ptr.hover && ptr.click;  // pointer click on the panel
    ptr.click = false;
    bool back = b[B_A] && !backHeld;
    backHeld = b[B_A];
    if (menuMode == MENU_HELP) {
        if (ok || back || fire || pclick) { menuMode = MENU_NONE; menuDirty = true; }
        return;
    }
    if (menuMode == MENU_CONTROLS) {
        int rows = N_ACTIONS + 3;
        if (ptr.hover) {
            int r = ptr.py / CELL_H - 2;
            if (r >= N_ACTIONS) r--;  // the gap row before "Applies to"
            if (r >= 0 && r < rows && r != controlsSel) { controlsSel = r; menuDirty = true; }
            if (pclick && r >= 0 && r < rows) ok = true;
        }
        bool clear = b[B_SELECT] && !favHeld;  // left X
        favHeld = b[B_SELECT];
        if (up) controlsSel = (controlsSel + rows - 1) % rows;
        if (down) controlsSel = (controlsSel + 1) % rows;
        if (controlsSel < N_ACTIONS) {
            int a = kActions[controlsSel];
            if (ok) { remapAction = a; remapArmed = false; }
            if (clear) { for (int i = 0; i < PH_COUNT; i++) if (mapping[i] == a) mapping[i] = ACT_NONE; saveMapping(); }
        } else if (controlsSel == N_ACTIONS && (ok || left || right) && gameLoaded) {
            // switching to "all games" drops this game's own file and goes back to the shared mapping
            if (controlsPerGame) { remove(controlsPath(true).c_str()); controlsPerGame = false; loadMapping(controlsPath(false)); }
            else { controlsPerGame = true; saveMapping(); }
        } else if (controlsSel == N_ACTIONS + 1 && ok) {
            memcpy(mapping, defaultMap, sizeof mapping); saveMapping(); showToast("Controls reset");
        } else if (controlsSel == N_ACTIONS + 2 && ok) {
            menuMode = MENU_PAUSE;
        }
        if (back) menuMode = MENU_PAUSE;
        menuDirty |= up || down || left || right || ok || back || clear;
        return;
    }
    if (menuMode == MENU_SLOTS) {
        if (ptr.hover && ptr.py >= 48 && ptr.py < 48 + 2 * 336) {
            int c = std::clamp((ptr.px - 16) / 504, 0, 1), r = std::clamp((ptr.py - 48) / 336, 0, 1);
            if (r * 2 + c != slotSel) { slotSel = r * 2 + c; menuDirty = true; }
            if (pclick) ok = true;
        }
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
        if (roms.empty()) { if (ok) { scanRoms(); buildLibView(); } menuDirty |= ok; return; }
        bool tabL = b[B_L] && !gripHeldL, tabR = b[B_R] && !gripHeldR, fav = b[B_SELECT] && !favHeld;
        if (ptr.hover && ptr.py < CELL_H && pclick)
            for (int t = 0; t < TAB_N; t++)
                if (ptr.px >= tabX0[t] && ptr.px < tabX1[t] && t != libTab) { libTab = t; romSel = 0; buildLibView(); menuDirty = true; }
        if (ptr.hover && ptr.py >= 40 && ptr.py < 40 + 2 * 336 && !libView.empty()) {
            int c = std::clamp((ptr.px - 16) / 248, 0, 3), r = (ptr.py - 40) / 336;
            int idx = (libRow0 + r) * 4 + c;
            if (idx < (int)libView.size()) {
                if (idx != romSel) { romSel = idx; menuDirty = true; }
                if (pclick) ok = true;
            }
        }
        gripHeldL = b[B_L]; gripHeldR = b[B_R]; favHeld = b[B_SELECT];
        if (tabL || tabR) { libTab = (libTab + (tabR ? 1 : TAB_N - 1)) % TAB_N; romSel = 0; buildLibView(); }
        int n = (int)libView.size();
        if (n) {
            if (up) romSel = std::max(0, romSel - 4);
            if (down) romSel = std::min(n - 1, romSel + 4);
            if (left) romSel = std::max(0, romSel - 1);
            if (right) romSel = std::min(n - 1, romSel + 1);
            if (fav) { auto &g = stats[libView[romSel]]; g.fav = !g.fav; saveStats(); if (libTab == TAB_FAV) buildLibView(); }
            if (ok) {
                std::string pick = libView[romSel];
                bool firstTime = stats[pick].seconds < 1;
                if (loadGame(pick)) { menuMode = firstTime ? MENU_HELP : MENU_NONE; resetMenuInput(); }
                else showToast("Could not load that ROM");
            }
        }
        if (back && gameLoaded) menuMode = MENU_PAUSE;
        menuDirty |= up || down || left || right || ok || back || tabL || tabR || fav;
        return;
    }
    // pause menu (current page)
    const std::vector<int> &items = pausePages[pausePage];
    int n = (int)items.size();
    pauseSel = std::clamp(pauseSel, 0, n - 1);
    if (up) pauseSel = (pauseSel + n - 1) % n;
    if (down) pauseSel = (pauseSel + 1) % n;
    int delta = right ? 1 : left ? -1 : 0;
    if (ptr.hover) {
        int k = ptr.py / CELL_H - 2;
        if (k >= 0 && k < n) {
            if (k != pauseSel) { pauseSel = k; menuDirty = true; }
            if (pclick) {
                int it = items[k];
                if (adjustable(it)) delta = ptr.px >= 30 * CELL_W + 80 ? 1 : ptr.px >= 30 * CELL_W ? -1 : 1;  // click "<" or ">"
                else ok = true;
            }
        }
    }
    int item = items[pauseSel];
    bool changed = false;
    if ((delta && (adjustable(item) || toggle(item))) || (ok && toggle(item))) {
        changed = true;
        switch (item) {
        case P_SPEED: {
            static const float speeds[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
            int k = 2;
            for (int j = 0; j < 5; j++) if (fabsf(speeds[j] - cfg.speed) < 0.01f) k = j;
            cfg.speed = speeds[std::clamp(k + (delta ? delta : 1), 0, 4)];
            break;
        }
        case P_DEPTH: cfg.strength = std::clamp(cfg.strength + 0.25f * delta, 0.0f, 3.0f); break;
        case P_CONV: cfg.convergence += 0.5f * delta; break;
        case P_3D: flip(cfg.stereoOn); break;
        case P_MODE7: flip(cfg.mode7Ramp); break;
        case P_SWAP: flip(cfg.swapEyes); break;
        case P_ROOM: flip(cfg.room); break;
        case P_STYLE: cfg.box = (float)(((int)lrintf(cfg.box) + (delta < 0 ? 2 : 1)) % 3); break;
        case P_LOOK: flip(cfg.popLook); break;
        case P_SKY: flip(cfg.sky); break;
        case P_TABLE: setTabletop(!on(cfg.table)); break;
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
    if (ok && !toggle(item)) {
        switch (item) {
        case P_RESUME: menuMode = MENU_NONE; break;
        case P_CONTROLS: menuMode = MENU_CONTROLS; controlsSel = 0; break;
        case P_RESET:
            markSeatHere();
            defaultPlacement(); saveGlobal();
            showToast("Moved in front of you");
            break;
        case P_SEAT: markSeatAndTell(); break;
        case P_HANG:  // a framed window at eye height in front of you; carry it to a wall with the bar
            cfg.table = 0; cfg.box = 2; cfg.sky = 1; cfg.popLook = 1;
            if (hasRoomView()) cfg.room = 1;
            cfg.screenWidth = 1.2f; cfg.distance = 1.6f;
            markSeatHere();  // hanging it in front of you defines where you are sitting
            defaultPlacement(); saveGlobal(); newFrame = frame.valid;
            showToast("Point at the bar under the frame and hold the trigger to carry it to a wall");
            break;
        case P_SAVE: menuMode = MENU_SLOTS; slotSaving = true; break;
        case P_LOAD: menuMode = MENU_SLOTS; slotSaving = false; break;
        case P_SHOT: takeScreenshot(); break;
        case P_CLIP: saveClip(); break;
        case P_GAMES: scanRoms(); saveStats(); buildLibView(); menuMode = MENU_ROMS; placeMenu(); break;
        case P_ABOUT: menuMode = MENU_ABOUT; aboutTop = 0; break;
        case P_PAGE_PICTURE: pausePage = 1; pauseSel = 1; break;
        case P_PAGE_ROOM: pausePage = 2; pauseSel = 1; break;
        case P_BACK: pausePage = 0; pauseSel = 0; break;
        }
    }
    if (back) { if (pausePage) { pausePage = 0; pauseSel = 0; } else menuMode = MENU_NONE; }
    menuDirty |= up || down || delta || ok || back || changed;
}
