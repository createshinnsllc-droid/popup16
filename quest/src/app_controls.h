// PopUp16 Quest app: controls: mapping files and remapping.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- controls: mapping files and remapping
static const int kActions[] = {B_B, B_Y, B_A, B_X, B_L, B_R, B_START, B_SELECT, ACT_REWIND, ACT_FAST, ACT_SHOT, ACT_CLIP};
static const int N_ACTIONS = sizeof(kActions) / sizeof(kActions[0]);
static const char *actionName(int a) {
    switch (a) {
    case B_B: return "B (jump)"; case B_Y: return "Y (run)"; case B_A: return "A"; case B_X: return "X";
    case B_L: return "L"; case B_R: return "R"; case B_START: return "Start"; case B_SELECT: return "Select";
    case ACT_REWIND: return "Rewind (hold)"; case ACT_FAST: return "Fast-forward (hold)";
    case ACT_SHOT: return "Screenshot"; case ACT_CLIP: return "Save last 30 s";
    default: return "-";
    }
}
static int remapAction = ACT_NONE;    // action waiting for a button press
static bool remapArmed = false, remapJustDone = false;
static std::string controlsPath(bool perGame);
static void loadMapping(const std::string &path) {
    memcpy(mapping, defaultMap, sizeof mapping);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char name[64]; int a;
    while (fscanf(f, " %63[^=]=%d", name, &a) == 2)
        for (int i = 0; i < PH_COUNT; i++) if (!strcmp(physNames[i], name)) mapping[i] = a;
    fclose(f);
}
static void saveMapping() {
    FILE *f = fopen(controlsPath(controlsPerGame).c_str(), "w");
    if (!f) return;
    for (int i = 0; i < PH_COUNT; i++) fprintf(f, "%s=%d\n", physNames[i], mapping[i]);
    fclose(f);
}
static std::string boundTo(int action) {  // "right A, pad A"
    std::string r;
    for (int i = 0; i < PH_COUNT; i++)
        if (mapping[i] == action) r += (r.empty() ? "" : ", ") + std::string(physNames[i]);
    return r.empty() ? "(none)" : r;
}
static std::vector<std::string> controlsHelp() {
    std::vector<std::string> l = {"CONTROLS", "", "Move ............. either thumbstick (gamepad: D-pad)"};
    for (int k = 0; k < N_ACTIONS; k++) {
        std::string n = actionName(kActions[k]);
        n += " " + std::string(std::max<int>(1, 17 - (int)n.size()), '.') + " ";
        l.push_back(n + boundTo(kActions[k]));
    }
    l.push_back("PopUp16 menu ..... left Y (gamepad: menu, or Select+Start)");
    l.push_back("");
    l.push_back("Change any of these: menu > Controls & remapping.");
    l.push_back("Press any button to play.");
    return l;
}
// waiting for the button to assign: everything must be released first, then the next press wins
static void remapCapture() {
    if (remapJustDone) {  // let go of the assigned button before menus react again
        bool any = false;
        for (bool d : physDown) any |= d;
        if (!any) remapJustDone = false;
        return;
    }
    bool any = false;
    for (bool d : physDown) any |= d;
    if (!remapArmed) { if (!any) remapArmed = true; return; }
    for (int i = 0; i < PH_COUNT; i++)
        if (physDown[i]) {
            mapping[i] = remapAction;
            saveMapping();
            trace("remap: %s -> %s", physNames[i], actionName(remapAction));
            remapAction = ACT_NONE;
            remapJustDone = true;
            return;
        }
}
