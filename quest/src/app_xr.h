// PopUp16 Quest app: OpenXR / EGL.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

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
static bool hasRoomView() { return hasPassthrough; }
static XrPassthroughFB passthrough = XR_NULL_HANDLE;
static XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
static bool passthroughRunning = false;
static bool recenterPending = false;  // re-place the menu once the new head pose is known
static void resetPassthrough();
static Swap menuSwap, cursorSwap;
static XrActionSet actionSet;
static XrAction actA, actB, actX, actY, actTrigL, actTrigR, actGripL, actGripR, actMenu, actStickL, actStickR, actClickL, actClickR;
static XrAction actPoseL, actPoseR, actAimL, actAimR;
static XrSpace handSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}, aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
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
    actAimL = makeAction("aim_left", XR_ACTION_TYPE_POSE_INPUT);
    actAimR = makeAction("aim_right", XR_ACTION_TYPE_POSE_INPUT);
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
        {actAimL, path("/user/hand/left/input/aim/pose")},
        {actAimR, path("/user/hand/right/input/aim/pose")},
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
        sci.action = h == 0 ? actAimL : actAimR;
        XRCHECK(xrCreateActionSpace(session, &sci, &aimSpace[h]));
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
    stickX = sx; stickY = sy;
    physDown[PH_RA] = getBool(actA);
    physDown[PH_RB] = getBool(actB);
    physDown[PH_RTRIG] = getFloat(actTrigR) > 0.5f;
    physDown[PH_RGRIP] = getFloat(actGripR) > 0.5f;
    physDown[PH_RCLICK] = getBool(actClickR);
    physDown[PH_LX] = getBool(actX);
    physDown[PH_LTRIG] = getFloat(actTrigL) > 0.5f;
    physDown[PH_LGRIP] = getFloat(actGripL) > 0.5f;
    physDown[PH_LMENU] = getBool(actMenu);
    physDown[PH_LCLICK] = getBool(actClickL);
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
    resetPassthrough();
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
    if (makeSwap(cursorSwap, 32, 32)) {  // the pointer dot: a white disc with a dark rim
        std::vector<uint32_t> dot(32 * 32, 0u);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                float d = sqrtf((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f));
                if (d < 11) dot[y * 32 + x] = 0xffffffffu;
                else if (d < 15) dot[y * 32 + x] = 0xff302820u;
            }
        uploadSwap(cursorSwap, dot.data(), 32, 32);
    }
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
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, e.w, e.h);  // stencil: window style
            LOGI("eye %d: %dx%d", eye, e.w, e.h);
        }
    }
    initActions();
    return true;
}

// room passthrough (XR_FB_passthrough): created once, started only while "your room" is chosen
// Returns true only when the room is actually running. A start failure leaves it off (and is
// reported once, not once per frame) so the caller can fall back to an opaque scene.
// Room-view objects and entry points belong to one OpenXR instance and session. Quest can start the
// app's main loop again inside the same process (leaving to Home and coming back), so they are reset
// whenever a new instance is created and destroyed with the session; stale handles otherwise fail
// with XR_ERROR_HANDLE_INVALID and the room never comes back.
static PFN_xrCreatePassthroughFB createPt;
static PFN_xrCreatePassthroughLayerFB createLayer;
static PFN_xrPassthroughStartFB startPt;
static PFN_xrPassthroughPauseFB pausePt;
static PFN_xrPassthroughLayerResumeFB resumeLayer;
static PFN_xrPassthroughLayerPauseFB pauseLayer;
static PFN_xrDestroyPassthroughFB destroyPt;
static PFN_xrDestroyPassthroughLayerFB destroyLayer;
static void resetPassthrough() {  // forget everything from a previous instance (do not call into it)
    createPt = nullptr; createLayer = nullptr; startPt = nullptr; pausePt = nullptr;
    resumeLayer = nullptr; pauseLayer = nullptr; destroyPt = nullptr; destroyLayer = nullptr;
    passthrough = XR_NULL_HANDLE; passthroughLayer = XR_NULL_HANDLE; passthroughRunning = false;
}
static void destroyPassthrough() {  // with the session still alive, before it is destroyed
    if (passthroughLayer && destroyLayer) destroyLayer(passthroughLayer);
    if (passthrough && destroyPt) destroyPt(passthrough);
    passthroughLayer = XR_NULL_HANDLE; passthrough = XR_NULL_HANDLE; passthroughRunning = false;
}

static bool setPassthrough(bool want) {
    if (!hasPassthrough) { passthroughRunning = false; return false; }
    static XrResult lastFail = XR_SUCCESS;
    if (!createPt) {
        xrGetInstanceProcAddr(instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&createPt);
        xrGetInstanceProcAddr(instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&createLayer);
        xrGetInstanceProcAddr(instance, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&startPt);
        xrGetInstanceProcAddr(instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&pausePt);
        xrGetInstanceProcAddr(instance, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction *)&resumeLayer);
        xrGetInstanceProcAddr(instance, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction *)&pauseLayer);
        xrGetInstanceProcAddr(instance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction *)&destroyPt);
        xrGetInstanceProcAddr(instance, "xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction *)&destroyLayer);
        if (!createPt || !createLayer || !startPt || !pausePt || !resumeLayer || !pauseLayer) { hasPassthrough = false; passthroughRunning = false; return false; }
    }
    if (want == passthroughRunning) return passthroughRunning;
    if (want) {
        static double nextTry = 0;  // a room that is not available now must not be retried every frame
        if (nowSec() < nextTry) return false;
        if (!passthrough) {
            XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
            if (XR_FAILED(createPt(session, &pci, &passthrough))) { trace("passthrough unavailable"); hasPassthrough = false; return false; }
        }
        if (!passthroughLayer) {
            XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = passthrough;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (XR_FAILED(createLayer(session, &lci, &passthroughLayer))) { trace("passthrough layer failed"); hasPassthrough = false; return false; }
        }
        XrResult sr = startPt(passthrough);
        if (XR_SUCCEEDED(sr)) sr = resumeLayer(passthroughLayer);
        if (XR_FAILED(sr)) {  // your room is not available in this state: stay opaque and look again soon
            if (sr != lastFail) trace("passthrough start failed (%d)", (int)sr);
            if (sr == XR_ERROR_HANDLE_INVALID) {  // objects from an earlier session: build fresh ones next try
                passthroughLayer = XR_NULL_HANDLE; passthrough = XR_NULL_HANDLE;
            }
            lastFail = sr;
            nextTry = nowSec() + 1.0;
            passthroughRunning = false;
            return false;
        }
        lastFail = XR_SUCCESS;
        nextTry = 0;
        passthroughRunning = true;
        trace("passthrough on");
    } else {
        // Pause in the order the runtime expects, and only touch handles that exist: a layer left
        // running across a session stop is never resolvable again, which is what leaves the room
        // permanently broken (the runtime reports a failed layer lookup every single frame).
        if (passthroughLayer) pauseLayer(passthroughLayer);
        if (passthrough) pausePt(passthrough);
        passthroughRunning = false;
        trace("passthrough off");
    }
    return passthroughRunning;
}

static void requestRefreshRate() {
    if (!hasRefreshExt) return;
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&enumRates);
    xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&requestRate);
    if (!enumRates || !requestRate) return;
    uint32_t n = 0;
    if (XR_FAILED(enumRates(session, 0, &n, nullptr)) || n == 0) { trace("display refresh: none advertised"); return; }
    std::vector<float> rates(n);
    if (XR_FAILED(enumRates(session, n, &n, rates.data()))) return;
    std::string list;
    for (float r : rates) { char b[16]; snprintf(b, sizeof b, "%s%.0f", list.empty() ? "" : ",", r); list += b; }
    float best = 0;
    for (float r : rates) if (fabsf(r - 120.0f) < 0.5f) best = r;  // 120 Hz shows 60 fps games without judder
    if (best == 0) for (float r : rates) if (fabsf(r - 90.0f) > 0.5f && r > best) best = r;
    if (best > 0) { XrResult rr = requestRate(session, best); LOGI("display refresh %.0f Hz", best); trace("display refresh %.0f Hz of [%s] (request %d)", best, list.c_str(), (int)rr); }
    else trace("display refresh: offered [%s], none requested", list.c_str());
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
                XrResult br = xrBeginSession(session, &bi);
                if (XR_SUCCEEDED(br)) { sessionRunning = true; requestRefreshRate(); }
                else trace("xrBeginSession failed (%d)", (int)br);
            } else if (sessionState == XR_SESSION_STATE_STOPPING) {
                setPassthrough(false);  // the room has to stop with the session, or the runtime can
                                        // never resolve the layer again (it fails every frame after)
                xrEndSession(session); sessionRunning = false; saveSram(); saveResume();
            } else if (sessionState == XR_SESSION_STATE_IDLE) {
                setPassthrough(false);
            } else if (sessionState == XR_SESSION_STATE_EXITING || sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                ANativeActivity_finish(app->activity);
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // Recentering moves the room coordinates with you: the game, its seat and its bar keep their
            // place relative to the new centre (it stays in front of you), and an open menu follows.
            trace("recentered");
            recenterPending = true;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            ANativeActivity_finish(app->activity);
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}
