#!/bin/zsh
# Installs SNES3D on a USB-connected Quest, copies the ROM library and starts the app.
# usage: install_quest.sh [rom folder]   (default: /Volumes/AI MAIN DRIVE/snes-usa-english)
set -e
cd "$(dirname "$0")"
ADB=$HOME/Library/Android/sdk/platform-tools/adb
ROMS=${1:-"/Volumes/AI MAIN DRIVE/snes-usa-english"}
PKG=com.createshinns.snes3d
DEST=/sdcard/Android/data/$PKG/files/roms
$ADB get-state >/dev/null 2>&1 || { echo "No headset found: plug in the Quest, put it on, and accept 'Allow USB debugging'."; exit 1; }
$ADB install -r build/snes3d.apk
# the app must create its own folders (Android 14 denies it access to adb-created ones)
OWNER=$($ADB shell "stat -c %U $DEST 2>/dev/null" | tr -d '\r')
if [[ "$OWNER" != u0_a* ]]; then
  $ADB shell rm -rf $DEST
  $ADB shell am start -S -n $PKG/android.app.NativeActivity >/dev/null
  for i in {1..20}; do $ADB shell "[ -d $DEST ]" && break; sleep 0.5; done
  $ADB shell am force-stop $PKG
fi
echo "copying ROMs from $ROMS ..."
$ADB push --sync "$ROMS"/*.sfc $DEST/ | tail -1
$ADB shell am start -n $PKG/android.app.NativeActivity
echo "started; logs: $ADB logcat -s SNES3D snes9x OpenXR"
