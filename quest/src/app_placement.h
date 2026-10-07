// PopUp16 Quest app: placement.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

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
