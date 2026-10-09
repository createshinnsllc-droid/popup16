#!/bin/zsh
# Builds the patched snes9x libretro core, the PopUp16 frontend, and ~/Applications/PopUp16.app.
set -e
cd "$(dirname "$0")"
brew list sdl2 >/dev/null 2>&1 || brew install sdl2
# the core Makefile does not track header changes, so always rebuild it clean
make -C snes9x/libretro platform=osx clean >/dev/null
make -C snes9x/libretro -j8 platform=osx
clang++ -std=c++20 -O2 -Wall -Wno-unused-parameter -I/opt/homebrew/include/SDL2 -Isnes9x/libretro \
  frontend/popup16.cpp -L/opt/homebrew/lib -lSDL2 -lz -o frontend/popup16
APP=~/Applications/PopUp16.app
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp frontend/popup16 snes9x/libretro/snes9x_libretro.dylib "$APP/Contents/Resources/"
# the Snes9x license and copyright must travel with every copy, so the app carries all notices
python3 tools/make_notices.py >/dev/null
cp LICENSE THIRD_PARTY_NOTICES.txt "$APP/Contents/Resources/"
cat > "$APP/Contents/MacOS/PopUp16" <<'SH'
#!/bin/zsh
RES="$(dirname "$0")/../Resources"
ROM="$1"
if [[ -z "$ROM" ]]; then
  ROM=$(osascript -e 'POSIX path of (choose file with prompt "Pick a SNES ROM (.sfc / .smc / .zip)")') || exit 0
fi
if [[ ! -f "$ROM" || ! -r "$ROM" ]]; then
  LOG=$(tail -n 5 ~/Library/Logs/PopUp16.log 2>/dev/null)
  osascript -e 'on run argv' -e 'display alert "PopUp16 could not open this ROM" message ("File: " & (item 1 of argv) & return & return & (item 2 of argv))' -e 'end run' "$ROM" "$LOG" >/dev/null 2>&1
  exit 1
fi
exec "$RES/popup16" "$RES/snes9x_libretro.dylib" "$ROM" >> ~/Library/Logs/PopUp16.log 2>&1
SH
chmod +x "$APP/Contents/MacOS/PopUp16"
cat > "$APP/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>PopUp16</string>
<key>CFBundleIdentifier</key><string>com.createshinns.popup16</string>
<key>CFBundleExecutable</key><string>PopUp16</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PL
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed; the app still runs from this Mac"
echo "built $APP"
