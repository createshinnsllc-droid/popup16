#!/bin/zsh
# Builds the patched snes9x libretro core, the SNES3D frontend, and ~/Applications/SNES3D.app.
set -e
cd "$(dirname "$0")"
brew list sdl2 >/dev/null 2>&1 || brew install sdl2
# the core Makefile does not track header changes, so always rebuild it clean
make -C snes9x/libretro platform=osx clean >/dev/null
make -C snes9x/libretro -j8 platform=osx
clang++ -std=c++20 -O2 -Wall -Wno-unused-parameter -I/opt/homebrew/include/SDL2 -Isnes9x/libretro \
  frontend/snes3d.cpp -L/opt/homebrew/lib -lSDL2 -o frontend/snes3d
APP=~/Applications/SNES3D.app
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp frontend/snes3d snes9x/libretro/snes9x_libretro.dylib "$APP/Contents/Resources/"
cat > "$APP/Contents/MacOS/SNES3D" <<'SH'
#!/bin/zsh
RES="$(dirname "$0")/../Resources"
ROM="$1"
if [[ -z "$ROM" ]]; then
  ROM=$(osascript -e 'POSIX path of (choose file with prompt "Pick a SNES ROM (.sfc / .smc / .zip)")') || exit 0
fi
exec "$RES/snes3d" "$RES/snes9x_libretro.dylib" "$ROM" >> ~/Library/Logs/SNES3D.log 2>&1
SH
chmod +x "$APP/Contents/MacOS/SNES3D"
cat > "$APP/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>SNES3D</string>
<key>CFBundleIdentifier</key><string>com.createshinns.snes3d</string>
<key>CFBundleExecutable</key><string>SNES3D</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PL
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "built $APP"
