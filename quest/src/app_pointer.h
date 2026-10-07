// PopUp16 Quest app: pointing: menus and the grab bar.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- pointing: menus and the grab bar
// Each controller casts a ray. On a menu it hovers and clicks (trigger). While playing, pointing at the
// bar under the game and holding the trigger carries the game; the thumbstick resizes it meanwhile.
static XrVector3f menuPos{0, 0, -1.4f};
static Quat menuRot{0, 0, 0, 1};
static const float MENU_WM = 1.2f, MENU_HM = 1.2f * MENU_H / MENU_W;
static void placeMenu() {  // in front of where you are looking
    float fx, fz; yawFwd(fx, fz);
    menuPos = {headX + fx * 1.3f, headY - 0.08f, headZ + fz * 1.3f};
    menuRot = {0, sinf(headYaw / 2), 0, cosf(headYaw / 2)};
}
static int pointHand = 1;                     // hand that last clicked (right by default)
static float trigPrev[2] = {0, 0};
static int grabHand = -1;                      // hand carrying the game by its bar
static XrPosef grabStart;
static XrVector3f grabPos0; static Quat grabRot0;
static bool barHover[2] = {false, false};

static XrVector3f v3(float x, float y, float z) { return {x, y, z}; }
static XrVector3f vadd(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static XrVector3f vsub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static XrVector3f vmul(XrVector3f a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static float vdot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static XrVector3f vcross(XrVector3f a, XrVector3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static XrVector3f vnorm(XrVector3f a) { float n = sqrtf(vdot(a, a)); return n > 1e-6f ? vmul(a, 1 / n) : v3(0, 0, -1); }

// ray against a rectangle in a frame (pos, rot); returns local x, y on hit
static bool rayRect(XrVector3f o, XrVector3f d, XrVector3f pos, Quat rot, float hw, float hh, float &lx, float &ly, float &t) {
    Quat inv = qconj(rot);
    XrVector3f lo = qrot(inv, vsub(o, pos)), ld = qrot(inv, d);
    if (ld.z > -1e-4f && ld.z < 1e-4f) return false;
    t = -lo.z / ld.z;
    if (t <= 0) return false;
    lx = lo.x + ld.x * t; ly = lo.y + ld.y * t;
    return fabsf(lx) <= hw && fabsf(ly) <= hh;
}
static void addRay(XrVector3f o, XrVector3f d, float len, const float *col) {
    XrVector3f e = vadd(o, vmul(d, len));
    XrVector3f side = vmul(vnorm(vcross(d, fabsf(d.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0))), 0.0018f);
    render::FlatShape sh;
    XrVector3f a = vsub(o, side), b = vadd(o, side), c = vadd(e, side), f = vsub(e, side);
    sh.tris = {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, a.x, a.y, a.z, c.x, c.y, c.z, f.x, f.y, f.z};
    memcpy(sh.color, col, 16);
    renderer.shapes.push_back(sh);
}
// the bar sits centred under the game's front plane
static void barFrame(XrVector3f &pos, Quat &rot, float &hw, float &hh) {
    rot = qnorm({cfg.qx, cfg.qy, cfg.qz, cfg.qw});
    float W = cfg.screenWidth, H = W * 3.0f / 4.0f;
    hw = std::clamp(0.3f * W, 0.12f, 0.6f) / 2;
    hh = std::clamp(0.03f * W, 0.012f, 0.05f) / 2;
    float gap = 0.02f + 0.02f * W + (cfg.box > 1.5f ? std::max(0.03f, 0.05f * W) + 0.01f : 0.0f);  // below the frame
    pos = vadd(v3(cfg.px, cfg.py, cfg.pz), qrot(rot, v3(0, -H / 2 - gap - hh, 0.004f)));
}
static void pointerUpdate(XrTime t) {
    XrVector3f org[2], dir[2]; bool valid[2];
    Quat aq[2];
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        valid[h] = aimSpace[h] && XR_SUCCEEDED(xrLocateSpace(aimSpace[h], localSpace, t, &loc)) &&
                   (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (!valid[h]) continue;
        aq[h] = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
        org[h] = loc.pose.position;
        dir[h] = qrot(aq[h], v3(0, 0, -1));
    }
    float tv[2] = {getFloat(actTrigL), getFloat(actTrigR)};
    bool pressed[2], released[2];
    for (int h = 0; h < 2; h++) { pressed[h] = tv[h] > 0.7f && trigPrev[h] <= 0.7f; released[h] = tv[h] < 0.3f; trigPrev[h] = tv[h]; }
    static const float rayCol[4] = {0.85f, 0.92f, 1.0f, 1.0f}, rayDim[4] = {0.45f, 0.5f, 0.6f, 1.0f};

    if (menuMode != MENU_NONE && menuMode != MENU_ARRANGE) {
        bool hit[2] = {false, false}; float lx[2], ly[2], tt[2];
        for (int h = 0; h < 2; h++)
            if (valid[h]) hit[h] = rayRect(org[h], dir[h], menuPos, menuRot, MENU_WM / 2, MENU_HM / 2, lx[h], ly[h], tt[h]);
        for (int h = 0; h < 2; h++) if (pressed[h] && hit[h]) pointHand = h;
        if (!hit[pointHand] && hit[1 - pointHand]) pointHand = 1 - pointHand;
        int h = pointHand;
        ptr.hover = hit[h];
        if (hit[h]) {
            int px = (int)((lx[h] / MENU_WM + 0.5f) * MENU_W), py = (int)((0.5f - ly[h] / MENU_HM) * MENU_H);
            if (px != ptr.px || py != ptr.py) { ptr.px = px; ptr.py = py; }
            if (pressed[h]) ptr.click = true;
        }
        for (int k = 0; k < 2; k++)
            if (valid[k]) addRay(org[k], dir[k], hit[k] ? tt[k] : 1.5f, k == h && hit[k] ? rayCol : rayDim);
        return;
    }
    ptr.hover = false;
    if (!gameLoaded || menuMode == MENU_ARRANGE) return;
    // the grab bar
    XrVector3f bp; Quat br; float hw, hh;
    barFrame(bp, br, hw, hh);
    float bt[2];
    for (int h = 0; h < 2; h++) {
        float lx, ly;
        barHover[h] = valid[h] && rayRect(org[h], dir[h], bp, br, hw * 1.6f, hh * 3.0f, lx, ly, bt[h]);
        if (barHover[h] && pressed[h] && grabHand < 0) {
            grabHand = h;
            grabStart.position = org[h];
            grabStart.orientation = {aq[h].x, aq[h].y, aq[h].z, aq[h].w};
            grabPos0 = v3(cfg.px, cfg.py, cfg.pz);
            grabRot0 = qnorm({cfg.qx, cfg.qy, cfg.qz, cfg.qw});
        }
    }
    if (grabHand >= 0) {
        int h = grabHand;
        if (!valid[h] || released[h]) {
            grabHand = -1;
            cfg.table = cfg.table;  // keep the mode; only the placement changed
            cfg.distance = std::clamp(sqrtf((cfg.px - headX) * (cfg.px - headX) + (cfg.pz - headZ) * (cfg.pz - headZ)), 0.4f, 8.0f);
            saveGlobal();
        } else {
            // carry: the game keeps its offset and turns with the controller
            Quat qs = {grabStart.orientation.x, grabStart.orientation.y, grabStart.orientation.z, grabStart.orientation.w};
            Quat dq = qnorm(qmul(aq[h], qconj(qs)));
            XrVector3f r = qrot(dq, vsub(grabPos0, grabStart.position));
            cfg.px = org[h].x + r.x; cfg.py = org[h].y + r.y; cfg.pz = org[h].z + r.z;
            Quat nr = qnorm(qmul(dq, grabRot0));
            cfg.qx = nr.x; cfg.qy = nr.y; cfg.qz = nr.z; cfg.qw = nr.w;
            // the stick of the carrying hand resizes
            XrVector2f st = getStick(h == 0 ? actStickL : actStickR);
            if (fabsf(st.y) > 0.3f) cfg.screenWidth = std::clamp(cfg.screenWidth * (1.0f + 0.015f * st.y), 0.3f, 8.0f);
            stickX = stickY = 0;  // the game does not see this stick while carrying
        }
    }
    // the trigger used on the bar does not reach the game
    for (int h = 0; h < 2; h++)
        if (barHover[h] || grabHand == h) physDown[h == 0 ? PH_LTRIG : PH_RTRIG] = false;
    // draw the bar (and the ray of a hand pointing at it)
    static const float barIdle[4] = {0.42f, 0.44f, 0.5f, 1}, barLit[4] = {0.55f, 0.75f, 1.0f, 1}, barHeld[4] = {0.3f, 0.6f, 1.0f, 1};
    render::FlatShape sh;
    XrVector3f c[4] = {vadd(bp, qrot(br, v3(-hw, -hh, 0))), vadd(bp, qrot(br, v3(hw, -hh, 0))),
                       vadd(bp, qrot(br, v3(hw, hh, 0))), vadd(bp, qrot(br, v3(-hw, hh, 0)))};
    sh.tris = {c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z,
               c[0].x, c[0].y, c[0].z, c[2].x, c[2].y, c[2].z, c[3].x, c[3].y, c[3].z};
    memcpy(sh.color, grabHand >= 0 ? barHeld : (barHover[0] || barHover[1]) ? barLit : barIdle, 16);
    renderer.shapes.push_back(sh);
    for (int h = 0; h < 2; h++)
        if (valid[h] && (barHover[h] || grabHand == h)) addRay(org[h], dir[h], grabHand == h ? 0.25f : bt[h], rayCol);
}

// Window style: the game world lies behind a picture frame; it is only visible through the opening,
// so the frame reads as a hole in the wall you can look into from any angle.
static void buildWindow(const float *m) {
    renderer.opening.clear();
    renderer.inside.clear();
    if (cfg.box < 1.5f || !gameLoaded) return;
    float W = cfg.screenWidth, H = W * 3.0f / 4.0f;
    auto P = [&](float x, float y, float z) {  // model space to room
        return v3(m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]);
    };
    auto quad = [&](std::vector<float> &out, XrVector3f a, XrVector3f b, XrVector3f c, XrVector3f d) {
        out.insert(out.end(), {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z});
    };
    quad(renderer.opening, P(-W / 2, -H / 2, 0), P(W / 2, -H / 2, 0), P(W / 2, H / 2, 0), P(-W / 2, H / 2, 0));
    // picture frame: four moulding pieces around the opening, standing slightly out from the wall,
    // with a thin light inner lip so the edge of the opening reads clearly
    float f = std::max(0.03f, 0.05f * W), lip = std::max(0.006f, 0.008f * W), z = 0.012f;
    render::FlatShape frameSh, lipSh;
    static const float wood[4] = {0.24f, 0.16f, 0.10f, 1}, lipCol[4] = {0.55f, 0.45f, 0.32f, 1};
    memcpy(frameSh.color, wood, 16); memcpy(lipSh.color, lipCol, 16);
    float x0 = -W / 2 - lip, x1 = W / 2 + lip, y0 = -H / 2 - lip, y1 = H / 2 + lip;
    quad(lipSh.tris, P(x0, y0, z), P(x1, y0, z), P(x1, -H / 2, z), P(x0, -H / 2, z));
    quad(lipSh.tris, P(x0, H / 2, z), P(x1, H / 2, z), P(x1, y1, z), P(x0, y1, z));
    quad(lipSh.tris, P(x0, -H / 2, z), P(-W / 2, -H / 2, z), P(-W / 2, H / 2, z), P(x0, H / 2, z));
    quad(lipSh.tris, P(W / 2, -H / 2, z), P(x1, -H / 2, z), P(x1, H / 2, z), P(W / 2, H / 2, z));
    float X0 = x0 - f, X1 = x1 + f, Y0 = y0 - f, Y1 = y1 + f, zf = z + 0.004f;
    quad(frameSh.tris, P(X0, Y0, zf), P(X1, Y0, zf), P(X1, y0, zf), P(X0, y0, zf));
    quad(frameSh.tris, P(X0, y1, zf), P(X1, y1, zf), P(X1, Y1, zf), P(X0, Y1, zf));
    quad(frameSh.tris, P(X0, y0, zf), P(x0, y0, zf), P(x0, y1, zf), P(X0, y1, zf));
    quad(frameSh.tris, P(x1, y0, zf), P(X1, y0, zf), P(X1, y1, zf), P(x1, y1, zf));
    renderer.shapes.push_back(frameSh);
    renderer.shapes.push_back(lipSh);
    // the box behind the frame: four dark walls running back from the opening edges, each a shade
    // different so the corners read
    // the walls flare out exactly along the sheet edges seen from the usual spot (distance D in front),
    // so head-on they are edge-on and invisible; from the side they fill what the game never drew
    float D = placementDepth();
    float L = 24.0f * D, k = (D + L) / D;
    static const float wl[4][4] = {{0.10f, 0.09f, 0.09f, 1}, {0.07f, 0.065f, 0.065f, 1}, {0.085f, 0.08f, 0.08f, 1}, {0.12f, 0.11f, 0.105f, 1}};
    XrVector3f a = P(-W / 2, -H / 2, 0), b = P(W / 2, -H / 2, 0), c = P(W / 2, H / 2, 0), d = P(-W / 2, H / 2, 0);
    XrVector3f A = P(-W / 2 * k, -H / 2 * k, -L), B = P(W / 2 * k, -H / 2 * k, -L), C = P(W / 2 * k, H / 2 * k, -L), Dd = P(-W / 2 * k, H / 2 * k, -L);
    render::FlatShape w[4];
    quad(w[0].tris, a, b, B, A);   // floor
    quad(w[1].tris, d, c, C, Dd);  // ceiling
    quad(w[2].tris, a, d, Dd, A);  // left
    quad(w[3].tris, b, c, C, B);   // right
    for (int k = 0; k < 4; k++) { memcpy(w[k].color, wl[k], 16); renderer.inside.push_back(w[k]); }
}
