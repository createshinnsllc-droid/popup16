// PopUp16 Quest app: audio.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- audio
// Single-producer (emulator thread) / single-consumer (AAudio callback) ring of stereo int16 frames.
static const int OUT_RATE = 48000;
static const uint32_t RING = 1 << 15;
static int16_t ring[RING * 2];
static std::atomic<uint32_t> ringW{0}, ringR{0};
static double resampleStep = 32040.5 / OUT_RATE, resamplePos = 0.0;
static int16_t prevL = 0, prevR = 0;
static AAudioStream *aaStream = nullptr;
static bool muteAudio = false;  // rewind plays back silently
static capture::Recorder recorder;  // last 30 s of frames and sound for clips
static void recordAudio(int16_t l, int16_t r) { if (!muteAudio) recorder.pushAudio(l, r); }

static uint32_t ringFill() { return ringW.load(std::memory_order_acquire) - ringR.load(std::memory_order_acquire); }
static void recordAudio(int16_t l, int16_t r);
static void pushOut(int16_t l, int16_t r) {
    recordAudio(l, r);
    uint32_t w = ringW.load(std::memory_order_relaxed);
    if (w - ringR.load(std::memory_order_acquire) >= RING) return;  // full: drop
    ring[(w & (RING - 1)) * 2] = l; ring[(w & (RING - 1)) * 2 + 1] = r;
    ringW.store(w + 1, std::memory_order_release);
}
static void pushIn(int16_t l, int16_t r) {  // linear resample core rate -> 48 kHz
    while (resamplePos <= 1.0) {
        pushOut((int16_t)(prevL + (l - prevL) * resamplePos), (int16_t)(prevR + (r - prevR) * resamplePos));
        resamplePos += resampleStep;
    }
    resamplePos -= 1.0;
    prevL = l; prevR = r;
}
static aaudio_data_callback_result_t aaCallback(AAudioStream *, void *, void *audioData, int32_t numFrames) {
    int16_t *out = (int16_t *)audioData;
    uint32_t r = ringR.load(std::memory_order_relaxed), w = ringW.load(std::memory_order_acquire);
    for (int i = 0; i < numFrames; i++) {
        if (r != w) { out[i * 2] = ring[(r & (RING - 1)) * 2]; out[i * 2 + 1] = ring[(r & (RING - 1)) * 2 + 1]; r++; }
        else { out[i * 2] = out[i * 2 + 1] = 0; }
    }
    ringR.store(r, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void startAudio() {
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, OUT_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aaCallback, nullptr);
    if (AAudioStreamBuilder_openStream(b, &aaStream) != AAUDIO_OK) aaStream = nullptr;
    AAudioStreamBuilder_delete(b);
    if (aaStream) AAudioStream_requestStart(aaStream);
    LOGI("audio %s", aaStream ? "started" : "unavailable");
}
static void stopAudio() {
    if (aaStream) { AAudioStream_requestStop(aaStream); AAudioStream_close(aaStream); aaStream = nullptr; }
}
