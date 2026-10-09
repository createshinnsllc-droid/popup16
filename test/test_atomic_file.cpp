// Host-side tests for shared/atomic_file.h, the save-data write used by the Mac frontend.
//
//   c++ -std=c++20 -O2 -Wall -Wextra -o /tmp/test_atomic test/test_atomic_file.cpp && /tmp/test_atomic

#include "../shared/atomic_file.h"

#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(const char *what, bool ok) {
    std::printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

bool readText(const fs::path &p, std::string &out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool hasText(const fs::path &p, const std::string &want) {
    std::string got;
    return readText(p, got) && got == want;
}

bool saveText(const fs::path &p, const std::string &text, std::string *reason = nullptr) {
    return atomicfile::write(p.string(), text.data(), text.size(), reason);
}

}  // namespace

int main() {
    fs::path dir = fs::temp_directory_path() / ("popup16-atomic-test-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path target = dir / "game.srm";
    const fs::path bak = dir / "game.srm.bak";
    const fs::path tmp = dir / "game.srm.tmp";

    // first write: no previous file, so nothing to keep as .bak
    check("first write succeeds", saveText(target, "one"));
    check("first write creates the file", hasText(target, "one"));
    check("first write leaves no .bak", !fs::exists(bak));
    check("first write leaves no .tmp", !fs::exists(tmp));

    // successful write replaces the file and keeps the previous one as .bak
    check("second write succeeds", saveText(target, "two"));
    check("second write replaces the file", hasText(target, "two"));
    check(".bak keeps the previous file", hasText(bak, "one"));
    check("second write leaves no .tmp", !fs::exists(tmp));

    check("third write succeeds", saveText(target, "three"));
    check("third write replaces the file", hasText(target, "three"));
    check(".bak rotates to the newest previous file", hasText(bak, "two"));

    // failed write: the temp file cannot be created, so the target and .bak must not change
    fs::create_directory(tmp);
    std::string reason;
    check("write fails when the temp file cannot be created", !saveText(target, "four", &reason));
    check("failed write sets a reason", !reason.empty());
    check("failed write leaves the old file intact", hasText(target, "three"));
    check("failed write leaves .bak intact", hasText(bak, "two"));
    fs::remove(tmp);

    // failed write: the backup step cannot create its temp file, so the target must not move
    fs::create_directory(dir / "game.srm.bak.tmp");
    reason.clear();
    check("write fails when the .bak temp file cannot be created", !saveText(target, "five", &reason));
    check("backup failure sets a reason", !reason.empty());
    check("backup failure leaves the old file intact", hasText(target, "three"));
    check("backup failure leaves .bak intact", hasText(bak, "two"));
    check("backup failure leaves no .tmp", !fs::exists(tmp));
    fs::remove(dir / "game.srm.bak.tmp");

    // a later write still works after the failures
    check("write after failures succeeds", saveText(target, "six"));
    check("write after failures replaces the file", hasText(target, "six"));
    check(".bak holds the file replaced by the last good write", hasText(bak, "three"));

    // failed first write: nothing is created at the target
    const fs::path fresh = dir / "fresh.state";
    fs::create_directory(dir / "fresh.state.tmp");
    check("failed first write creates no target", !saveText(fresh, "x") && !fs::exists(fresh));
    fs::remove(dir / "fresh.state.tmp");

    fs::remove_all(dir);
    std::printf("%s\n", failures ? "FAILED" : "all atomic_file tests passed");
    return failures ? 1 : 0;
}
