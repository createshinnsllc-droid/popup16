// Rewind history: the emulator state is snapshotted every few frames; each snapshot is stored as
// the XOR difference to the one before it, run-length packed (states change very little between
// snapshots, so most of each difference is zeros). Going back one step is current XOR difference.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

class Rewind {
public:
    explicit Rewind(size_t budgetBytes = 64u << 20) : budget(budgetBytes) {}

    void clear() { steps.clear(); current.clear(); used = 0; }
    size_t depth() const { return steps.size(); }
    size_t bytes() const { return used; }

    // record a new snapshot
    void push(const std::vector<uint8_t> &state) {
        if (current.size() == state.size()) {
            steps.push_back(pack(state, current));
            used += steps.back().size();
            while (used > budget && !steps.empty()) { used -= steps.front().size(); steps.pop_front(); }
        } else {
            clear();  // first snapshot, or the game changed
        }
        current = state;
    }
    // step back: out receives the previous snapshot; false when history is exhausted
    bool pop(std::vector<uint8_t> &out) {
        if (steps.empty()) return false;
        unpack(steps.back(), current);  // current ^= diff turns it into the previous snapshot
        used -= steps.back().size();
        steps.pop_back();
        out = current;
        return true;
    }

private:
    std::deque<std::vector<uint8_t>> steps;
    std::vector<uint8_t> current;
    size_t budget, used = 0;

    static void put32(std::vector<uint8_t> &v, uint32_t x) { v.insert(v.end(), (uint8_t *)&x, (uint8_t *)&x + 4); }
    // (zero run, literal length, literal bytes)* of a XOR b
    static std::vector<uint8_t> pack(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
        std::vector<uint8_t> out;
        size_t n = a.size(), i = 0;
        while (i < n) {
            size_t z = i;
            while (z < n && a[z] == b[z]) z++;
            size_t l = z;
            // a literal run ends at 8+ equal bytes in a row
            size_t eq = 0;
            while (l < n && eq < 8) { eq = (a[l] == b[l]) ? eq + 1 : 0; l++; }
            if (eq >= 8) l -= eq;
            put32(out, (uint32_t)(z - i));
            put32(out, (uint32_t)(l - z));
            for (size_t k = z; k < l; k++) out.push_back(a[k] ^ b[k]);
            i = l;
        }
        return out;
    }
    static void unpack(const std::vector<uint8_t> &d, std::vector<uint8_t> &s) {
        size_t i = 0, p = 0;
        while (p + 8 <= d.size()) {
            uint32_t z, l;
            memcpy(&z, &d[p], 4); memcpy(&l, &d[p + 4], 4); p += 8;
            i += z;
            for (uint32_t k = 0; k < l; k++) s[i + k] ^= d[p + k];
            i += l; p += l;
        }
    }
};
