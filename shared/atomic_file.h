// Atomic replacement for save data (SRAM, save states). Header-only, POSIX.
//
// atomicfile::write() never leaves a half-written file at the target path:
//   1. the new bytes go to <path>.tmp and are flushed to disk;
//   2. the current file, if there is one, is copied to <path>.bak (via <path>.bak.tmp);
//   3. <path>.tmp is renamed over <path>, a single atomic step.
// A failure before step 3 leaves <path> with its previous contents and removes the temp files.
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace atomicfile {

namespace detail {

inline bool fail(std::string *reason, const std::string &what, int err) {
    if (reason) *reason = err ? what + ": " + strerror(err) : what;
    return false;
}

// Gets the bytes out of the OS cache. macOS fsync() does not reach the disk, so F_FULLFSYNC comes first.
inline bool syncFile(FILE *f) {
    if (fflush(f) != 0) return false;
    int fd = fileno(f);
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return true;  // some volumes refuse it: fall back to fsync
#endif
    return fsync(fd) == 0;
}

// Writes n bytes to a new file at path. On failure the partial file is removed.
inline bool writeWhole(const std::string &path, const void *data, size_t n, std::string *reason) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return fail(reason, "cannot open " + path + " for writing", errno);
    errno = 0;
    bool ok = (n == 0 || fwrite(data, 1, n, f) == n) && syncFile(f);
    int err = errno;
    if (fclose(f) != 0 && ok) { ok = false; err = errno; }
    if (!ok) {
        remove(path.c_str());
        return fail(reason, "incomplete write " + path, err ? err : EIO);
    }
    return true;
}

inline bool readWhole(const std::string &path, std::vector<uint8_t> &out, std::string *reason) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return fail(reason, "cannot read " + path, errno);
    out.clear();
    uint8_t buf[1 << 16];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + got);
    bool ok = !ferror(f);
    int err = errno;
    fclose(f);
    return ok || fail(reason, "cannot read " + path, err ? err : EIO);
}

// Renames from over to, removing from if the rename fails.
inline bool renameOver(const std::string &from, const std::string &to, std::string *reason) {
    if (rename(from.c_str(), to.c_str()) == 0) return true;
    int err = errno;
    remove(from.c_str());
    return fail(reason, "cannot replace " + to, err);
}

}  // namespace detail

// Replaces the file at path with n bytes. Returns false, and sets *reason when given, if any step fails;
// path then still holds its previous contents. A successful write keeps the replaced file as path + ".bak".
inline bool write(const std::string &path, const void *data, size_t n, std::string *reason = nullptr) {
    const std::string tmp = path + ".tmp", bak = path + ".bak";
    if (!detail::writeWhole(tmp, data, n, reason)) return false;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        std::vector<uint8_t> old;
        if (!detail::readWhole(path, old, reason) ||
            !detail::writeWhole(bak + ".tmp", old.data(), old.size(), reason) ||
            !detail::renameOver(bak + ".tmp", bak, reason)) {
            remove(tmp.c_str());
            return false;
        }
    } else if (errno != ENOENT) {
        remove(tmp.c_str());
        return detail::fail(reason, "cannot inspect " + path, errno);
    }

    return detail::renameOver(tmp, path, reason);
}

}  // namespace atomicfile
