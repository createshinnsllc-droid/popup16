// PopUp16 Quest app: input.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- input
enum { B_B, B_Y, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_A, B_X, B_L, B_R, B_COUNT };  // RETRO_DEVICE_ID_JOYPAD order
// Physical buttons (Touch controllers and gamepad) and what each does in a game. Left Y and the
// gamepad's menu button always open the PopUp16 menu; menus always use the fixed buttons below.
enum Phys { PH_RA, PH_RB, PH_RTRIG, PH_RGRIP, PH_RCLICK, PH_LX, PH_LTRIG, PH_LGRIP, PH_LMENU, PH_LCLICK,
            PH_PAD_A, PH_PAD_B, PH_PAD_X, PH_PAD_Y, PH_PAD_L1, PH_PAD_R1, PH_PAD_L2, PH_PAD_R2, PH_PAD_START, PH_PAD_SELECT, PH_COUNT };
static const char *physNames[PH_COUNT] = {"right A", "right B", "right trigger", "right grip", "right stick click",
    "left X", "left trigger", "left grip", "left menu", "left stick click",
    "pad A", "pad B", "pad X", "pad Y", "pad L1", "pad R1", "pad L2", "pad R2", "pad Start", "pad Select"};
enum { ACT_NONE = -1, ACT_REWIND = 100, ACT_FAST = 101, ACT_SHOT = 102, ACT_CLIP = 103 };  // otherwise a B_* SNES button
static const int defaultMap[PH_COUNT] = {B_B, B_A, B_Y, B_R, B_START, B_SELECT, B_X, B_L, B_START, ACT_REWIND,
                                         B_B, B_A, B_Y, B_X, B_L, B_R, ACT_REWIND, ACT_FAST, B_START, B_SELECT};
static int mapping[PH_COUNT];
static bool physDown[PH_COUNT];
static bool controlsPerGame = false;  // mapping saved for this game only
static void loadMapping(const std::string &path);
static bool padMenu = false, xrMenu = false;
static float navX = 0, navY = 0, trigRValue = 0;  // menu: right stick X, left stick Y, right trigger
static float stickX = 0, stickY = 0;              // strongest Touch thumbstick, for the D-pad
static bool padDpad[4];                           // up down left right
static float padAxisX = 0, padAxisY = 0;
static bool joypad[B_COUNT];

static void dpadFrom(bool *b) {  // D-pad: either Touch stick, the gamepad's D-pad or left stick
    b[B_UP] = stickY > 0.5f || padDpad[0] || padAxisY < -0.5f;
    b[B_DOWN] = stickY < -0.5f || padDpad[1] || padAxisY > 0.5f;
    b[B_LEFT] = stickX < -0.5f || padDpad[2] || padAxisX < -0.5f;
    b[B_RIGHT] = stickX > 0.5f || padDpad[3] || padAxisX > 0.5f;
}
static bool wantShot = false, wantClip = false;  // capture buttons held this frame
static void gameButtons(bool *b, bool &rewind, bool &fast) {  // what the game sees, through the mapping
    memset(b, 0, sizeof(bool) * B_COUNT);
    dpadFrom(b);
    rewind = fast = wantShot = wantClip = false;
    for (int i = 0; i < PH_COUNT; i++) {
        if (!physDown[i]) continue;
        int a = mapping[i];
        if (a >= 0 && a < B_COUNT) b[a] = true;
        else if (a == ACT_REWIND) rewind = true;
        else if (a == ACT_FAST) fast = true;
        else if (a == ACT_SHOT) wantShot = true;
        else if (a == ACT_CLIP) wantClip = true;
    }
}
static void menuButtons(bool *b) {  // fixed: A select, B back, grips/L1-R1 tabs, left X / pad Y favourite
    memset(b, 0, sizeof(bool) * B_COUNT);
    dpadFrom(b);
    b[B_B] = physDown[PH_RA] || physDown[PH_PAD_A];
    b[B_A] = physDown[PH_RB] || physDown[PH_PAD_B];
    b[B_L] = physDown[PH_LGRIP] || physDown[PH_PAD_L1];
    b[B_R] = physDown[PH_RGRIP] || physDown[PH_PAD_R1];
    b[B_SELECT] = physDown[PH_LX] || physDown[PH_PAD_Y];
}
static int keyToPhys(int32_t key) {
    switch (key) {
    case AKEYCODE_BUTTON_A: return PH_PAD_A;
    case AKEYCODE_BUTTON_B: return PH_PAD_B;
    case AKEYCODE_BUTTON_X: return PH_PAD_X;
    case AKEYCODE_BUTTON_Y: return PH_PAD_Y;
    case AKEYCODE_BUTTON_L1: return PH_PAD_L1;
    case AKEYCODE_BUTTON_R1: return PH_PAD_R1;
    case AKEYCODE_BUTTON_L2: return PH_PAD_L2;
    case AKEYCODE_BUTTON_R2: return PH_PAD_R2;
    case AKEYCODE_BUTTON_START: return PH_PAD_START;
    case AKEYCODE_BUTTON_SELECT: return PH_PAD_SELECT;
    default: return -1;
    }
}
static int32_t onInput(android_app *, AInputEvent *e) {
    int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e);
        bool down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        if (key == AKEYCODE_BUTTON_MODE || key == AKEYCODE_BUTTON_THUMBL) { padMenu = down; return 1; }
        trace("gamepad key %d %s", key, down ? "down" : "up");
        int d = key == AKEYCODE_DPAD_UP ? 0 : key == AKEYCODE_DPAD_DOWN ? 1 : key == AKEYCODE_DPAD_LEFT ? 2 : key == AKEYCODE_DPAD_RIGHT ? 3 : -1;
        if (d >= 0) { padDpad[d] = down; return 1; }
        int ph = keyToPhys(key);
        if (ph >= 0) { physDown[ph] = down; return 1; }
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
