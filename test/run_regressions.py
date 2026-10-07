#!/usr/bin/env python3
"""Offline Mac checks; all writes stay under ignored test/output/.

Build a separate frontend (never run build.sh), then:
  python3 test/run_regressions.py --binary test/output/popup16 --rom-dir /path/to/owned/roms
  python3 test/run_regressions.py --binary test/output/popup16 --self-test
Missing fixtures are BLOCKED and make the run fail. No installs or downloads.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "test/output"
CASES = (
    ("dkc", "Donkey Kong Country.sfc"),
    ("contra", "Contra III - The Alien Wars.sfc"),
    ("earthworm", "Earthworm Jim.sfc"),
    ("ff3", "Final Fantasy III.sfc"),
    ("axelay", "Axelay.sfc"),
    ("battle-cars", "Battle Cars.sfc"),
    ("f1-roc", "F1 ROC II - Race of Champions.sfc"),
    ("layer-test", None),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def new_run(prefix):
    # Refuse symlink escapes rather than writing into an unexpected data folder.
    if (ROOT / "test").is_symlink() or OUTPUT.is_symlink():
        raise ValueError("test/output must not be a symlink")
    OUTPUT.mkdir(exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=prefix + "-", dir=OUTPUT))


def isolated_env(home):
    (home / "Library/Application Support").mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("POPUP16_", "DYLD_", "SDL_", "XDG_"))}
    env.update(HOME=str(home), TMPDIR=str(home), XDG_CONFIG_HOME=str(home),
               XDG_DATA_HOME=str(home), SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    return env


def execute(binary, core, rom, dest, frames, rewind=False, extra_env=None, output=None):
    dest.mkdir()
    env = isolated_env(dest / "home")
    env["POPUP16_AUTO"] = "1"
    if rewind:
        env["POPUP16_REWINDTEST"] = "1"
    else:
        env["POPUP16_PLANES"] = str(dest / "plane")
    if extra_env:
        env.update(extra_env)
    cmd = [str(binary), str(core), str(rom), "--dump", str(frames), str(output or dest / "frame.bmp")]
    start = time.monotonic()
    try:
        result = subprocess.run(cmd, cwd=dest, env=env, capture_output=True, text=True, timeout=90)
        code, text = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as exc:
        code = None
        text = "TIMEOUT\n" + (exc.stdout or b"").decode(errors="replace") + (exc.stderr or b"").decode(errors="replace")
    (dest / "run.log").write_text(text)
    return {"exit_code": code, "text": text, "elapsed_seconds": round(time.monotonic() - start, 3),
            "command": cmd, "frames": frames, "mode": "rewind" if rewind else "exactness"}


def check_bmp(path):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP: " + path.name)
    size, offset = struct.unpack_from("<IxxxxI", data, 2)
    dib, w, h, planes, bits, compression = struct.unpack_from("<IiiHHI", data, 14)
    if dib != 40 or offset != 54 or planes != 1 or bits != 24 or compression != 0 or w <= 0 or h <= 0:
        raise ValueError("unsupported BMP header: " + path.name)
    expected = offset + ((w * 3 + 3) & ~3) * h
    if size != len(data) or len(data) != expected:
        raise ValueError("truncated BMP: " + path.name)
    return w, h


def verdict(result, dest):
    if result["exit_code"] != 0:
        raise ValueError("frontend exit: " + str(result["exit_code"]))
    text = result["text"]
    if result["mode"] == "rewind":
        matches = re.findall(r"rewind: (\d+) snapshots of (\d+) B, history \d+ B \([^\n]*?\), (\d+) steps back, (\d+) mismatches, restore (ok|FAILED)", text)
        expected = (result["frames"] + 2) // 3
        if len(matches) != 1:
            raise ValueError("missing or ambiguous rewind verdict")
        snapshots, size, steps, bad, restored = matches[0]
        if int(snapshots) != expected or int(size) <= 0 or int(steps) != expected - 1 or int(bad) or restored != "ok":
            raise ValueError("failed/incomplete rewind round trip")
        return {"snapshots": int(snapshots), "steps": int(steps), "mismatches": int(bad), "restore": restored}
    matches = re.findall(r"head-on mismatch (\d+) of (\d+) px", text)
    if len(matches) != 1 or int(matches[0][0]) != 0 or int(matches[0][1]) <= 0:
        raise ValueError("failed/missing exactness verdict")
    sizes = [check_bmp(dest / ("plane" + str(n) + ".bmp")) for n in range(5)]
    if len(set(sizes)) != 1 or sizes[0][0] * sizes[0][1] != int(matches[0][1]):
        raise ValueError("plane dimensions disagree with verdict")
    check_bmp(dest / "frame.bmp")
    if check_bmp(dest / "planelook.bmp") != sizes[0]:
        raise ValueError("look output dimensions disagree")
    if "mode7:" in text:
        check_bmp(dest / "planehd.bmp")
    return {"mismatches": 0, "pixels": int(matches[0][1]), "dimensions": sizes[0],
            "mode7": next((s for s in text.splitlines() if s.startswith("mode7:")), None)}


# A synthetic libretro core exercises the ACTUAL frontend's failure exits.
# No copyrighted fixtures, real saves or production core changes are required.
FAKE_CORE = r'''
#include "libretro.h"
#include <cstdlib>
#include <cstring>
#include <cstdint>
static retro_video_refresh_t video;
static uint16_t colors[512*224];
static uint8_t layers[512*224], depths[512*224];
static uint32_t state;
static bool mode(const char *s) { const char *v = getenv("FAKE_MODE"); return v && !strcmp(v,s); }
extern "C" {
void retro_init() { for (int i=0;i<512*224;i++) { colors[i]=0xf800; depths[i]=mode("mismatch")?0:43; } }
void retro_deinit() {}
void retro_set_environment(retro_environment_t) {}
void retro_set_video_refresh(retro_video_refresh_t cb) { video=cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t) {}
void retro_set_input_poll(retro_input_poll_t) {}
void retro_set_input_state(retro_input_state_t) {}
void retro_get_system_av_info(retro_system_av_info *av) { av->timing.fps=60; av->timing.sample_rate=32040; }
bool retro_load_game(const retro_game_info *) { return true; }
void retro_unload_game() {}
void retro_run() { ++state; video(colors,256,224,1024); }
void *retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }
size_t retro_serialize_size() { return sizeof(state); }
bool retro_serialize(void *p,size_t) { memcpy(p,&state,sizeof(state)); return !mode("snapshot-fail"); }
bool retro_unserialize(const void *p,size_t n) { if (n!=sizeof(state)||mode("restore-fail")) return false; memcpy(&state,p,n); return true; }
const uint8_t *snes3d_get_layers() { return layers; }
const uint8_t *snes3d_get_depths() { return depths; }
#ifndef OMIT_PLANES
void snes3d_enable_planes(int) {}
const uint16_t *snes3d_get_plane_color(int) { return colors; }
const uint8_t *snes3d_get_plane_z(int n) { static uint8_t empty[512*224]; return n==0?depths:empty; }
#endif
}
'''


def self_test(binary):
    out = new_run("self-test")
    source = out / "fake_core.cpp"
    source.write_text(FAKE_CORE)
    core = out / "fake_core.dylib"
    missing = out / "missing_exports.dylib"
    for target, flags in ((core, []), (missing, ["-DOMIT_PLANES"])):
        subprocess.run(["clang++", "-std=c++17", "-dynamiclib", "-I" + str(ROOT / "snes9x/libretro"),
                        *flags, str(source), "-o", str(target)], check=True)
    rom = out / "dummy.sfc"
    rom.write_bytes(b"synthetic fixture")

    class Checks(unittest.TestCase):
        def run_case(self, name, **kwargs):
            dest = out / name
            return execute(binary, kwargs.pop("core", core), rom, dest, kwargs.pop("frames", 12), **kwargs), dest

        def test_exactness_success(self):
            r, d = self.run_case("success")
            self.assertEqual(verdict(r, d)["pixels"], 57344)

        def test_reconstruction_failure_exit(self):
            r, _ = self.run_case("bad-pixels", extra_env={"FAKE_MODE": "mismatch"})
            self.assertEqual(r["exit_code"], 1)
            self.assertRegex(r["text"], r"head-on mismatch [1-9]\d*")

        def test_rewind_success(self):
            r, d = self.run_case("rewind", rewind=True)
            self.assertEqual(verdict(r, d)["steps"], 3)

        def test_snapshot_failure(self):
            r, _ = self.run_case("snapshot", rewind=True, extra_env={"FAKE_MODE": "snapshot-fail"})
            self.assertEqual(r["exit_code"], 1)
            self.assertIn("snapshot failed", r["text"])

        def test_restore_failure(self):
            r, _ = self.run_case("restore", rewind=True, extra_env={"FAKE_MODE": "restore-fail"})
            self.assertEqual(r["exit_code"], 1)
            self.assertIn("restore FAILED", r["text"])

        def test_insufficient_rewind(self):
            r, _ = self.run_case("short", rewind=True, frames=3)
            self.assertEqual(r["exit_code"], 1)

        def test_missing_exports(self):
            r, _ = self.run_case("exports", core=missing)
            self.assertEqual(r["exit_code"], 1)
            self.assertIn("requires patched", r["text"])

        def test_unwritable_dump(self):
            r, _ = self.run_case("write-fail", output=out / "absent/frame.bmp")
            self.assertEqual(r["exit_code"], 1)
            self.assertIn("cannot write", r["text"])

        def test_unwritable_planes(self):
            r, _ = self.run_case("planes-fail", extra_env={"POPUP16_PLANES": str(out / "absent/plane")})
            self.assertEqual(r["exit_code"], 1)

        def test_invalid_frames(self):
            for frames in (0, -1, "12oops", 100001):
                r, _ = self.run_case("invalid-" + str(frames), frames=frames)
                self.assertEqual(r["exit_code"], 1)

        def test_validator_rejects_false_success(self):
            for text in ("", "head-on mismatch 3 of 57344 px", "head-on mismatch 0 of 0 px"):
                with self.assertRaises(ValueError):
                    verdict({"exit_code": 0, "text": text, "mode": "exactness"}, out)
            for text in ("", "rewind: 4 snapshots of 4 B, history 1 B (1%), 2 steps back, 0 mismatches, restore ok",
                         "rewind: 4 snapshots of 4 B, history 1 B (1%), 3 steps back, 1 mismatches, restore ok",
                         "rewind: 4 snapshots of 4 B, history 1 B (1%), 3 steps back, 0 mismatches, restore FAILED"):
                with self.assertRaises(ValueError):
                    verdict({"exit_code": 0, "text": text, "mode": "rewind", "frames": 12}, out)

        def test_validator_rejects_missing_and_truncated_artifacts(self):
            r, d = self.run_case("artifacts")
            (d / "frame.bmp").write_bytes(b"BM")
            with self.assertRaises(ValueError): verdict(r, d)
            (d / "plane0.bmp").unlink()
            with self.assertRaises(FileNotFoundError): verdict(r, d)

        def test_environment_isolation(self):
            env = isolated_env(out / "isolated-home")
            self.assertNotEqual(env["HOME"], os.environ.get("HOME"))
            self.assertFalse(any(k.startswith(("POPUP16_", "DYLD_")) for k in env))
            self.assertTrue((Path(env["HOME"]) / "Library/Application Support").is_dir())

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Checks))
    (out / "summary.json").write_text(json.dumps({"tests": result.testsRun, "failures": len(result.failures),
                                                   "errors": len(result.errors)}, indent=2) + "\n")
    print("Self-test artifacts:", out)
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True, help="isolated frontend binary, not the installed app")
    parser.add_argument("--core", type=Path, default=ROOT / "snes9x/libretro/snes9x_libretro.dylib")
    parser.add_argument("--rom-dir", type=Path, help="read-only folder containing the seven owned baseline ROMs")
    parser.add_argument("--frames", type=int, default=2400)
    parser.add_argument("--rewind-frames", type=int, default=180)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.binary.is_file(): parser.error("frontend binary missing; build it locally first")
    if not 4 <= args.frames <= 100000 or not 4 <= args.rewind_frames <= 100000:
        parser.error("frame counts must be between 4 and 100000")
    binary, core = args.binary.resolve(), args.core.resolve()
    if args.self_test: return self_test(binary)
    if not core.is_file(): parser.error("patched core missing; no automatic setup will be run")
    out = new_run("regressions")
    fixture = out / "layertest.sfc"
    subprocess.run([sys.executable, str(ROOT / "test/make_test_rom.py"), str(fixture)], check=True)
    results = []
    for key, filename in CASES:
        rom = args.rom_dir / filename if filename and args.rom_dir else (fixture if not filename else None)
        if rom is None or not rom.is_file():
            results.append({"case": key, "status": "BLOCKED", "reason": "owned fixture missing"})
            print(key + ": BLOCKED (owned fixture missing)")
            continue
        rom = rom.resolve()
        original_hash = digest(rom)
        for rewind in (False, True):
            dest = out / (key + ("-rewind" if rewind else "-exactness"))
            result = execute(binary, core, rom, dest, args.rewind_frames if rewind else args.frames, rewind)
            result.update(case=key, rom_sha256=original_hash)
            try:
                result["verdict"] = verdict(result, dest)
                if digest(rom) != original_hash: raise ValueError("source ROM hash changed")
                result["status"] = "PASS"
            except (ValueError, OSError) as exc:
                result.update(status="FAIL", reason=str(exc))
            print(key + " " + result["mode"] + ": " + result["status"] + " " + str(result.get("verdict", result.get("reason"))), flush=True)
            result.pop("text")  # full log is retained separately
            results.append(result)
    (out / "summary.json").write_text(json.dumps({"binary_sha256": digest(binary), "core_sha256": digest(core),
                                                  "results": results}, indent=2) + "\n")
    print("Artifacts:", out)
    return 1 if any(r["status"] != "PASS" for r in results) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print("BLOCKED:", exc, file=sys.stderr)
        sys.exit(2)
