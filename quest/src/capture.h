// Capture: the last 30 seconds of frames and sound are kept in memory (frames as XOR differences,
// run-length packed, with a full frame every second), so "save clip" can write them out as an MP4
// (H.264 + AAC) with the headset's hardware encoders, on a background thread.
#pragma once
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../shared/frame_shape.h"

namespace capture {

static const double SECONDS = 30.0;
static const int AUDIO_RATE = 48000;

struct Frame {               // one emulated frame
    uint16_t w, h;
    bool key;                // full frame (diff against zeros) or diff against the previous frame
    std::vector<uint8_t> packed;
};

inline void packDiff(const uint8_t *a, const uint8_t *b, size_t n, std::vector<uint8_t> &out) {
    // (zero run, literal length, literal bytes)* of a XOR b; b == nullptr means against zeros
    out.clear();
    auto put32 = [&](uint32_t x) { out.insert(out.end(), (uint8_t *)&x, (uint8_t *)&x + 4); };
    size_t i = 0;
    while (i < n) {
        size_t z = i;
        while (z < n && a[z] == (b ? b[z] : 0)) z++;
        size_t l = z, eq = 0;
        while (l < n && eq < 8) { eq = (a[l] == (b ? b[l] : 0)) ? eq + 1 : 0; l++; }
        if (eq >= 8) l -= eq;
        put32((uint32_t)(z - i)); put32((uint32_t)(l - z));
        for (size_t k = z; k < l; k++) out.push_back(a[k] ^ (b ? b[k] : 0));
        i = l;
    }
}
inline void applyDiff(const std::vector<uint8_t> &d, uint8_t *s) {
    size_t i = 0, p = 0;
    while (p + 8 <= d.size()) {
        uint32_t z, l;
        memcpy(&z, &d[p], 4); memcpy(&l, &d[p + 4], 4); p += 8;
        i += z;
        for (uint32_t k = 0; k < l; k++) s[i + k] ^= d[p + k];
        i += l; p += l;
    }
}

class Recorder {
public:
    double fps = 60.0988;

    void pushFrame(const uint16_t *px, unsigned w, unsigned h) {
        std::lock_guard<std::mutex> l(m);
        size_t n = (size_t)w * h * 2;
        Frame f{(uint16_t)w, (uint16_t)h, false, {}};
        bool key = sinceKey >= 60 || prev.size() != n || frames.empty();
        packDiff((const uint8_t *)px, key ? nullptr : prev.data(), n, f.packed);
        f.key = key;
        sinceKey = key ? 1 : sinceKey + 1;
        prev.assign((const uint8_t *)px, (const uint8_t *)px + n);
        frames.push_back(std::move(f));
        // keep 30 s, and always start on a full frame
        while (frames.size() > (size_t)(SECONDS * fps)) {
            frames.pop_front();
            while (!frames.empty() && !frames.front().key) frames.pop_front();
        }
    }
    void pushAudio(int16_t l, int16_t r) {  // 48 kHz stereo, as played
        if (audio.empty()) audio.resize((size_t)(SECONDS * AUDIO_RATE) * 2);
        audio[aw * 2] = l; audio[aw * 2 + 1] = r;
        aw = (aw + 1) % (audio.size() / 2);
        if (aFill < audio.size() / 2) aFill++;
    }
    void clear() { std::lock_guard<std::mutex> l(m); frames.clear(); prev.clear(); aFill = 0; aw = 0; }
    double seconds() { std::lock_guard<std::mutex> l(m); return frames.size() / fps; }

    bool busy() const { return working.load(); }
    // result of the last save: 1 ok, -1 failed, 0 none; message for the user
    int takeResult(std::string &msg) {
        int r = result.exchange(0);
        if (r) { std::lock_guard<std::mutex> l(m); msg = resultMsg; }
        return r;
    }

    // start writing the buffered clip to path (returns false if one is already being written)
    bool save(const std::string &path) {
        if (working.exchange(true)) return false;
        std::deque<Frame> f;
        std::vector<int16_t> a;
        {
            std::lock_guard<std::mutex> l(m);
            f = frames;
            size_t n = aFill, cap = audio.size() / 2;
            a.resize(n * 2);
            for (size_t i = 0; i < n; i++) {  // oldest first
                size_t k = (aw + cap - n + i) % cap;
                a[i * 2] = audio[k * 2]; a[i * 2 + 1] = audio[k * 2 + 1];
            }
        }
        if (worker.joinable()) worker.join();
        worker = std::thread([this, f = std::move(f), a = std::move(a), path]() mutable {
            std::string msg;
            bool ok = encode(f, a, path, msg);
            { std::lock_guard<std::mutex> l(m); resultMsg = msg; }
            result = ok ? 1 : -1;
            working = false;
        });
        return true;
    }
    ~Recorder() { if (worker.joinable()) worker.join(); }

private:
    std::mutex m;
    std::deque<Frame> frames;
    std::vector<uint8_t> prev;
    int sinceKey = 0;
    std::vector<int16_t> audio;
    size_t aw = 0, aFill = 0;
    std::atomic<bool> working{false};
    std::atomic<int> result{0};
    std::string resultMsg;
    std::thread worker;

    struct Sample { std::vector<uint8_t> data; int64_t pts; uint32_t flags; };

    // drain encoded output; returns false if the encoder stops answering (end of stream never comes)
    static bool drain(AMediaCodec *c, std::vector<Sample> &out, AMediaFormat **fmt, bool untilEnd) {
        int idle = 0;
        while (true) {
            AMediaCodecBufferInfo info;
            ssize_t i = AMediaCodec_dequeueOutputBuffer(c, &info, untilEnd ? 10000 : 0);
            if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { if (*fmt) AMediaFormat_delete(*fmt); *fmt = AMediaCodec_getOutputFormat(c); continue; }
            if (i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                if (!untilEnd) return true;
                if (++idle > 300) return false;  // ~3 s without output
                continue;
            }
            idle = 0;
            if (i < 0) continue;
            size_t sz;
            uint8_t *buf = AMediaCodec_getOutputBuffer(c, i, &sz);
            if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) && info.size > 0)
                out.push_back({std::vector<uint8_t>(buf + info.offset, buf + info.offset + info.size), info.presentationTimeUs, info.flags});
            AMediaCodec_releaseOutputBuffer(c, i, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return true;
        }
    }
    // an input buffer, draining output while waiting; -1 after ~3 s
    static ssize_t nextInput(AMediaCodec *c, std::vector<Sample> &out, AMediaFormat **fmt) {
        for (int tries = 0; tries < 300; tries++) {
            ssize_t in = AMediaCodec_dequeueInputBuffer(c, 10000);
            if (in >= 0) return in;
            drain(c, out, fmt, false);
        }
        return -1;
    }

    bool encode(std::deque<Frame> &frames, std::vector<int16_t> &audioPcm, const std::string &path, std::string &msg) {
#define CLOG(...) __android_log_print(ANDROID_LOG_INFO, "PopUp16clip", __VA_ARGS__)
        CLOG("encode: %zu frames, %zu audio samples -> %s", frames.size(), audioPcm.size() / 2, path.c_str());
        if (frames.empty()) { msg = "Nothing to save yet"; return false; }
        // The encoder and the NV12 buffer below are sized from the first frame, so every later frame
        // must match it. A resolution change mid-recording (hires mode, a game switch) could otherwise
        // write past the end of that buffer; refuse the clip and say why instead. The rule itself lives
        // in shared/frame_shape.h so it can be unit tested without Android codecs.
        if (!frames::allMatchFirst(frames)) {
            for (const auto &fr : frames)
                if (fr.w != frames.front().w || fr.h != frames.front().h) {
                    CLOG("refusing to encode: frame %ux%u does not match the %ux%u encoder",
                         fr.w, fr.h, frames.front().w, frames.front().h);
                    break;
                }
            msg = "Clip skipped: the picture size changed while recording";
            return false;
        }
        const int S = 2;  // 2x pixels: 512x448 for a 256x224 game
        const int W = frames.front().w * S, H = frames.front().h * S;
        const int EW = (W + 15) & ~15, EH = (H + 15) & ~15;
        std::vector<Sample> vs, as;
        AMediaFormat *vfmt = nullptr, *afmt = nullptr;

        // ---- video: frames to NV12, H.264
        AMediaCodec *ve = AMediaCodec_createEncoderByType("video/avc");
        AMediaFormat *f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, EW);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, EH);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);  // YUV420 semi-planar (NV12)
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, 8000000);
        AMediaFormat_setFloat(f, AMEDIAFORMAT_KEY_FRAME_RATE, (float)fps);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        bool vok = ve && AMediaCodec_configure(ve, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
                   AMediaCodec_start(ve) == AMEDIA_OK;
        AMediaFormat_delete(f);
        CLOG("video encoder %s (%dx%d)", vok ? "started" : "FAILED", EW, EH);
        if (!vok) { msg = "Video encoder unavailable"; if (ve) AMediaCodec_delete(ve); return false; }
        std::vector<uint8_t> cur, nv12((size_t)EW * EH * 3 / 2);
        size_t idx = 0, total = frames.size();
        for (auto &fr : frames) {
            size_t n = (size_t)fr.w * fr.h * 2;
            if (fr.key || cur.size() != n) cur.assign(n, 0);
            applyDiff(fr.packed, cur.data());
            const uint16_t *px = (const uint16_t *)cur.data();
            // RGB565 to BT.601 limited-range NV12, each source pixel becoming an SxS block
            std::fill(nv12.begin(), nv12.begin() + (size_t)EW * EH, 16);
            std::fill(nv12.begin() + (size_t)EW * EH, nv12.end(), 128);
            for (int y = 0; y < fr.h; y++)
                for (int x = 0; x < fr.w; x++) {
                    uint16_t c = px[y * fr.w + x];
                    int r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
                    uint8_t Y = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    for (int dy = 0; dy < S; dy++) memset(&nv12[(size_t)(y * S + dy) * EW + x * S], Y, S);
                    if (S == 2) {  // one chroma sample per 2x2 block = per source pixel
                        uint8_t *uv = &nv12[(size_t)EW * EH + (size_t)y * EW + x * 2];
                        uv[0] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);  // Cb
                        uv[1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);   // Cr
                    }
                }
            ssize_t in = nextInput(ve, vs, &vfmt);
            if (in < 0) { CLOG("video encoder stalled at frame %zu", idx); break; }
            size_t cap;
            uint8_t *ib = AMediaCodec_getInputBuffer(ve, in, &cap);
            size_t len = std::min(cap, nv12.size());
            memcpy(ib, nv12.data(), len);
            int64_t pts = (int64_t)(idx * 1e6 / fps);
            AMediaCodec_queueInputBuffer(ve, in, 0, len, pts, ++idx == total ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
            drain(ve, vs, &vfmt, false);
            if (idx % 300 == 0) CLOG("video %zu/%zu frames, %zu samples out", idx, total, vs.size());
        }
        if (!drain(ve, vs, &vfmt, true)) CLOG("video encoder did not finish cleanly");
        CLOG("video done: %zu samples", vs.size());
        AMediaCodec_stop(ve); AMediaCodec_delete(ve);

        // ---- audio: the matching tail of the sound history, AAC
        double clipSec = total / fps;
        size_t want = std::min(audioPcm.size() / 2, (size_t)(clipSec * AUDIO_RATE));
        const int16_t *pcm = audioPcm.data() + (audioPcm.size() / 2 - want) * 2;
        AMediaCodec *ae = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, AUDIO_RATE);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, 160000);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_PROFILE, 2);  // AAC LC
        bool aok = want > 0 && ae && AMediaCodec_configure(ae, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
                   AMediaCodec_start(ae) == AMEDIA_OK;
        AMediaFormat_delete(f);
        if (aok) {
            size_t pos = 0;
            while (true) {
                ssize_t in = nextInput(ae, as, &afmt);
                if (in < 0) { CLOG("audio encoder stalled at %zu/%zu", pos, want); aok = false; break; }
                size_t cap;
                uint8_t *ib = AMediaCodec_getInputBuffer(ae, in, &cap);
                size_t frames4 = std::min((size_t)1024, std::min(cap / 4, want - pos));
                memcpy(ib, pcm + pos * 2, frames4 * 4);
                int64_t pts = (int64_t)(pos * 1e6 / AUDIO_RATE);
                pos += frames4;
                bool end = pos >= want;
                AMediaCodec_queueInputBuffer(ae, in, 0, frames4 * 4, pts, end ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
                if ((pos / 1024) % 300 == 0) CLOG("audio %zu/%zu, %zu samples out", pos, want, as.size());
                drain(ae, as, &afmt, false);
                if (end) break;
            }
            if (aok && !drain(ae, as, &afmt, true)) CLOG("audio encoder did not finish cleanly (%zu samples)", as.size());
            AMediaCodec_stop(ae);
        }
        if (ae) AMediaCodec_delete(ae);

        CLOG("audio %s: %zu samples", aok ? "done" : "skipped", as.size());
        // ---- mux, interleaved by time
        int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0664);
        if (fd < 0 || !vfmt) { msg = "Could not write the clip file"; if (fd >= 0) close(fd); return false; }
        AMediaMuxer *mx = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        ssize_t vt = AMediaMuxer_addTrack(mx, vfmt);
        ssize_t at = (afmt && !as.empty()) ? AMediaMuxer_addTrack(mx, afmt) : -1;
        AMediaMuxer_start(mx);
        size_t vi = 0, ai = 0;
        while (vi < vs.size() || (at >= 0 && ai < as.size())) {
            bool takeV = at < 0 || ai >= as.size() || (vi < vs.size() && vs[vi].pts <= as[ai].pts);
            Sample &sm = takeV ? vs[vi++] : as[ai++];
            AMediaCodecBufferInfo info{0, (int32_t)sm.data.size(), sm.pts, sm.flags & ~AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM};
            AMediaMuxer_writeSampleData(mx, takeV ? vt : at, sm.data.data(), &info);
        }
        AMediaMuxer_stop(mx);
        AMediaMuxer_delete(mx);
        close(fd);
        AMediaFormat_delete(vfmt);
        if (afmt) AMediaFormat_delete(afmt);
        char b[96];
        snprintf(b, sizeof b, "Clip saved (%.0f s)", clipSec);
        msg = b;
        return true;
    }
};

}  // namespace capture
