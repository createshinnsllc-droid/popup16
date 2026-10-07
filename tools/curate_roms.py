#!/usr/bin/env python3
"""Build a USA + English-translation SNES library from GoodSNES-style .7z/.zip sets.

For every archive, pick one ROM:
  1. the best USA dump ((U), (JU), (UE), (JUE)), verified [!] first, newest revision;
  2. otherwise the best English fan translation ([T+Eng], then [T-Eng]).
Archives with neither are listed as skipped. Bad dumps, hacks, fixes, trainers and
overdumps are never picked. Originals are left untouched.

usage: curate_roms.py <source dir> <dest dir>
"""
import os, re, subprocess, sys

SEVENZIP = next(p for p in ("/opt/homebrew/bin/7zz", "/usr/local/bin/7zz", "/opt/homebrew/bin/7z") if os.path.exists(p))
ROM_EXT = (".sfc", ".smc", ".swc", ".fig")
BAD = re.compile(r"\[(b|h|f|t|o|p|x)\d*[^\]]*\]|\((beta|proto|sample|demo|hack|pd)\b|competition|\[a\d*\]", re.I)
US = re.compile(r"\((U|JU|UE|JUE)\)")


def list_entries(path):
    if path.lower().endswith(".zip"):
        out = subprocess.run(["/usr/bin/unzip", "-Z1", path], capture_output=True, text=True).stdout
        return [l for l in out.splitlines() if l]
    out = subprocess.run([SEVENZIP, "l", "-ba", "-slt", path], capture_output=True, text=True).stdout
    return [l[7:] for l in out.splitlines() if l.startswith("Path = ")]


def version(name):
    m = re.search(r"\(V1\.(\d)\)", name)
    return int(m.group(1)) if m else 0


def tr_version(name):
    m = re.search(r"\[T[+-]Eng(\d+(?:\.\d+)?)", name)
    return float(m.group(1)) if m else 0.0


def pick(entries):
    roms = [e for e in entries if e.lower().endswith(ROM_EXT)]
    clean = [e for e in roms if not BAD.search(e)]
    us = [e for e in clean if US.search(e)]
    if us:
        return max(us, key=lambda e: ("[!]" in e, version(e), -len(e))), "usa"
    # translations carry a [T+Eng...] tag, so judge the rest of the name for badness
    tr = [e for e in roms if re.search(r"\[T[+-]Eng", e) and not BAD.search(re.sub(r"\[T[+-]Eng[^\]]*\]", "", e))]
    if tr:
        return max(tr, key=lambda e: ("[T+Eng" in e, tr_version(e), "-Emu" in e, version(e), -len(e))), "translation"
    return None, None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    counts = {"usa": 0, "translation": 0, "skipped": 0, "existing": 0}
    skipped = []
    for fn in sorted(os.listdir(src)):
        if not fn.lower().endswith((".7z", ".zip")):
            continue
        path = os.path.join(src, fn)
        entry, kind = pick(list_entries(path))
        if not entry:
            counts["skipped"] += 1
            skipped.append(fn)
            continue
        title = os.path.splitext(fn)[0]
        out = os.path.join(dst, title + (" [T+Eng]" if kind == "translation" else "") + ".sfc")
        if os.path.exists(out) and os.path.getsize(out) > 0:
            counts["existing"] += 1
            continue
        if path.lower().endswith(".zip"):
            data = subprocess.run(["/usr/bin/unzip", "-p", path, entry], capture_output=True).stdout
        else:
            data = subprocess.run([SEVENZIP, "e", "-so", path, entry], capture_output=True).stdout
        # strip 512-byte copier headers so every file is a plain .sfc image
        if len(data) % 1024 == 512:
            data = data[512:]
        if not data:
            counts["skipped"] += 1
            skipped.append(fn + " (extract failed)")
            continue
        with open(out, "wb") as f:
            f.write(data)
        counts[kind] += 1
        print(f"{kind:11s} {title}  <-  {entry}", flush=True)
    with open(os.path.join(dst, "_skipped.txt"), "w") as f:
        f.write("\n".join(skipped) + "\n")
    print("summary:", counts)


if __name__ == "__main__":
    main()
