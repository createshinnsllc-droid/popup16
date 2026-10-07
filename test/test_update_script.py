#!/usr/bin/env python3
"""Prove quest/update_app.sh is an app-only update.

Runs the real script against a fake adb that records every command it is asked to run, then
asserts that updating never deletes, copies, pushes, uninstalls or launches anything. This is the
check that must fail if someone reintroduces the destructive installer behaviour into an update.

  python3 test/test_update_script.py
"""
from pathlib import Path
import os
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "quest/update_app.sh"
FORBIDDEN = ("rm ", "rm -", "unlink", "cp ", "push", "uninstall", "am start", "am force-stop",
             "chmod", "mv ", "dd ", "format", "sdcard/Android/data/com.createshinns.popup16/files/roms")

FAKE_ADB = r"""#!/bin/sh
printf '%s\n' "$*" >> "$ADB_LOG"
case "$1" in
  get-state) exit 0 ;;
  install)   exit 0 ;;
  shell)     printf '%s\n' "covers" "input.log" "library.cfg" "roms" "saves" "settings.cfg"; exit 0 ;;
  *)         exit 0 ;;
esac
"""


def run(apk_exists=True):
    out = Path(tempfile.mkdtemp(prefix="update-test-"))
    log = out / "adb.log"
    fake = out / "adb"
    fake.write_text(FAKE_ADB)
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    apk = out / "popup16.apk"
    if apk_exists:
        apk.write_bytes(b"not a real apk, just a file the script can see")
    env = dict(os.environ, ADB=str(fake), ADB_LOG=str(log))
    proc = subprocess.run(["zsh", str(SCRIPT), str(apk)], capture_output=True, text=True, env=env)
    return proc, (log.read_text() if log.exists() else "")


def main():
    failures = []

    proc, log = run(apk_exists=True)
    print("--- exit", proc.returncode, "---")
    print(log.strip())
    if proc.returncode != 0:
        failures.append(f"update with an APK present should succeed, exit was {proc.returncode}")
    if "install -r" not in log:
        failures.append("the update never asked adb to install -r")
    lowered = log.lower()
    for bad in FORBIDDEN:
        if bad.lower() in lowered:
            failures.append(f"app-only update ran a forbidden command containing {bad!r}")

    # A missing APK must stop before any adb call at all.
    proc2, log2 = run(apk_exists=False)
    if proc2.returncode == 0:
        failures.append("a missing APK should fail")
    if log2.strip():
        failures.append("a missing APK still reached adb: " + log2.strip())

    if failures:
        for f in failures:
            print("FAIL:", f)
        return 1
    print("PASS: app-only update installs in place and never deletes, copies, uninstalls or launches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
