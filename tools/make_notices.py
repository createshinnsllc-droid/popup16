#!/usr/bin/env python3
"""Assembles THIRD_PARTY_NOTICES.txt from the actual license files of every component that is
compiled into SNES3D, and copies it into the Quest APK assets (shown by the in-app About page).
Run from the repo root after ./setup.sh: python3 tools/make_notices.py"""
import os
import subprocess
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NDK_GLUE = os.path.expanduser("~/Library/Android/sdk/ndk/27.2.12479018/sources/android/native_app_glue/android_native_app_glue.c")
AAR = os.path.join(ROOT, "quest/third_party/openxr_loader_for_android-1.1.63.aar")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().rstrip() + "\n"


def header_comment(path, last_line_contains):
    """Leading license comment of a source file, up to the line containing the marker."""
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        out.append(line.rstrip())
        if last_line_contains in line:
            break
    return "\n".join(out) + "\n"


def section(title, body):
    bar = "=" * 79
    return f"{bar}\n{title}\n{bar}\n\n{body}\n"


intro = textwrap.dedent("""\
    SNES3D - third-party notices
    ============================

    SNES3D is a stereoscopic / layered-3D frontend built on the Snes9x emulator core.
    It is free, non-commercial software. No games are included; use only game files
    you are legally entitled to use.

    Super NES and Super Nintendo Entertainment System are trademarks of Nintendo Co.,
    Limited and its subsidiary companies. This project is not affiliated with,
    endorsed by, or sponsored by Nintendo. Meta Quest is a trademark of Meta
    Platforms, Inc. OpenXR is a trademark of The Khronos Group Inc.

    Components and their licenses:

      Snes9x emulator core (modified: per-layer output for 3D)
          Snes9x license - free for non-commercial use; full text below
      snes_spc / SPC_DSP sound DSP and snes_ntsc filter (Shay Green)
          GNU LGPL 2.1 - full text below; complete source for this app is published
      libretro API header (The RetroArch team)            MIT-style license
      Khronos OpenXR loader for Android 1.1.63             Apache License 2.0
      Android NDK native_app_glue (The Android Open Source Project)
                                                         Apache License 2.0
      Spleen 8x16 bitmap font (Frederic Cambus)            BSD 2-Clause
      SDL 2 (Sam Lantinga; Mac frontend only)              zlib license

    Changes to Snes9x are published as snes9x-stereo3d.patch in the source
    repository, in line with the Snes9x authors' request that improvements be
    shared.
    """)

parts = [intro]
parts.append(section("Snes9x", read(os.path.join(ROOT, "snes9x/LICENSE"))))
parts.append(section("snes_spc / SPC_DSP (apu/bapu/dsp) - header",
                     header_comment(os.path.join(ROOT, "snes9x/apu/bapu/dsp/SPC_DSP.cpp"), "Inc., 51 Franklin")))
parts.append(section("snes_ntsc - GNU Lesser General Public License 2.1",
                     read(os.path.join(ROOT, "snes9x/filter/snes_ntsc-license.txt"))))
parts.append(section("libretro API header", header_comment(os.path.join(ROOT, "snes9x/libretro/libretro.h"), "DEALINGS IN THE SOFTWARE")))
parts.append(section("Android NDK native_app_glue", header_comment(NDK_GLUE, "limitations under the License")))
parts.append(section("Spleen font", read(os.path.join(ROOT, "tools/fonts/SPLEEN-LICENSE"))))
sdl = "/opt/homebrew/opt/sdl2/LICENSE.txt"
if os.path.exists(sdl):
    parts.append(section("SDL 2 (Mac frontend only)", read(sdl)))
apache = subprocess.run(["unzip", "-p", AAR, "META-INF/LICENSE"], capture_output=True, text=True).stdout
parts.append(section("Khronos OpenXR loader for Android - Apache License 2.0 and bundled notices", apache.rstrip() + "\n"))

text = "\n".join(parts)
with open(os.path.join(ROOT, "THIRD_PARTY_NOTICES.txt"), "w") as f:
    f.write(text)
os.makedirs(os.path.join(ROOT, "quest/assets"), exist_ok=True)
with open(os.path.join(ROOT, "quest/assets/NOTICES.txt"), "w") as f:
    f.write(text)
print(f"wrote THIRD_PARTY_NOTICES.txt ({len(text.splitlines())} lines)")
