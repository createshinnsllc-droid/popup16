// PopUp16 Quest app: library: playtime, favourites, covers.
// Part of main.cpp's single translation unit: main.cpp includes the app_*.h files in order and they
// share its file-level state, so this file is not meant to be included anywhere else.
#pragma once

// ---------------------------------------------------------------- library: playtime, favourites, covers
static void loadStats() {
    FILE *f = fopen((filesDir + "/library.cfg").c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {  // name \t seconds \t last played (unix time) \t favourite
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
        GameStats g; int fav = 0;
        if (sscanf(t1 + 1, "%lf\t%ld\t%d", &g.seconds, &g.lastPlayed, &fav) >= 2) { g.fav = fav; stats[line] = g; }
    }
    fclose(f);
}
static bool saveStats() {
    if (stats.empty()) return true;                 // nothing loaded yet: keep the existing library as it is
    std::string text;
    char line[512];
    for (auto &[n, g] : stats) {
        snprintf(line, sizeof line, "%s\t%.0f\t%ld\t%d\n", n.c_str(), g.seconds, g.lastPlayed, g.fav ? 1 : 0);
        text += line;
    }
    return writeFileAtomic(filesDir + "/library.cfg", text.data(), text.size());
}
static std::string playtimeText(double sec) {
    char b[32];
    if (sec < 60) return sec < 1 ? "new" : "<1m";
    int m = (int)(sec / 60);
    if (m < 60) snprintf(b, sizeof b, "%dm", m); else snprintf(b, sizeof b, "%dh %02dm", m / 60, m % 60);
    return b;
}
static std::string lastPlayedText(long t) {
    if (!t) return "";
    time_t now = time(nullptr);
    long days = (long)(now / 86400) - t / 86400;
    if (days <= 0) return "today";
    if (days == 1) return "yesterday";
    char b[32]; time_t tt = t; strftime(b, sizeof b, "%b %d", localtime(&tt));
    return b;
}
// covers: <files>/covers/<game>.png|.jpg (made by tools/make_covers.sh, your own art, or captured in play),
// decoded once at card size with the platform image decoder
static const int CARD_W = 224, CARD_H = 196;
static std::map<std::string, std::vector<uint32_t>> coverCache;
static const std::vector<uint32_t> *cover(const std::string &rom) {
    std::string stemName = rom.substr(0, rom.find_last_of('.'));
    auto it = coverCache.find(stemName);
    if (it != coverCache.end()) return it->second.empty() ? nullptr : &it->second;
    if (coverCache.size() > 96) coverCache.clear();
    std::vector<uint32_t> &px = coverCache[stemName];
    for (const char *ext : {".png", ".jpg", ".jpeg", ".webp"}) {
        std::vector<uint8_t> data;
        if (!readFile(coverDir + "/" + stemName + ext, data)) continue;
        AImageDecoder *dec = nullptr;
        if (AImageDecoder_createFromBuffer(data.data(), data.size(), &dec) != ANDROID_IMAGE_DECODER_SUCCESS) continue;
        // Show the art at its own aspect ratio. Decoding straight to the card size squashed every
        // cover whose shape was not exactly 224x196, which distorted titles and faces. The image is
        // scaled to fit the card instead and centred, with warm bars filling the difference.
        const AImageDecoderHeaderInfo *hdr = AImageDecoder_getHeaderInfo(dec);
        const int srcW = (int)AImageDecoderHeaderInfo_getWidth(hdr);
        const int srcH = (int)AImageDecoderHeaderInfo_getHeight(hdr);
        const place::Size fit = place::fitInside({srcW, srcH}, {CARD_W, CARD_H});
        if (fit.w <= 0 || fit.h <= 0) { AImageDecoder_delete(dec); continue; }
        const place::Size at = place::centreInside(fit, {CARD_W, CARD_H});
        AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888);
        AImageDecoder_setTargetSize(dec, fit.w, fit.h);
        size_t stride = AImageDecoder_getMinimumStride(dec);
        std::vector<uint8_t> buf(stride * (size_t)fit.h);
        bool ok = AImageDecoder_decodeImage(dec, buf.data(), stride, buf.size()) == ANDROID_IMAGE_DECODER_SUCCESS;
        AImageDecoder_delete(dec);
        if (!ok) continue;
        px.assign((size_t)CARD_W * CARD_H, 0xff3a2a20u);  // same warm tone as a card with no art
        for (int y = 0; y < fit.h; y++)
            memcpy(&px[(size_t)(at.h + y) * CARD_W + at.w], &buf[(size_t)y * stride], (size_t)fit.w * 4);
        for (auto &c : px) c |= 0xff000000u;
        return &px;
    }
    return nullptr;
}
