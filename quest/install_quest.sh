#!/bin/zsh
# Installs PopUp16 on a USB-connected Quest, copies the ROM library and starts the app.
# usage: install_quest.sh <rom folder>   (folder of .sfc/.smc files you own)
set -e
cd "$(dirname "$0")"
ADB=$HOME/Library/Android/sdk/platform-tools/adb
ROMS=${1:?usage: install_quest.sh <rom folder>}
PKG=com.createshinns.popup16
DEST=/sdcard/Android/data/$PKG/files/roms
$ADB get-state >/dev/null 2>&1 || { echo "No headset found: plug in the Quest, put it on, and accept 'Allow USB debugging'."; exit 1; }
$ADB install -r build/popup16.apk
# the app must create its own folders (Android 14 denies it access to adb-created ones)
OWNER=$($ADB shell "stat -c %U $DEST 2>/dev/null" | tr -d '\r')
if [[ "$OWNER" != u0_a* ]]; then
  $ADB shell rm -rf $DEST
  $ADB shell am start -S -n $PKG/android.app.NativeActivity >/dev/null
  for i in {1..20}; do $ADB shell "[ -d $DEST ]" && break; sleep 0.5; done
  $ADB shell am force-stop $PKG
fi
# carry saves over from the earlier package name (com.createshinns.snes3d), once
OLD=/sdcard/Android/data/com.createshinns.snes3d/files/saves
if $ADB shell "[ -d $OLD ] && [ ! -e $DEST/../saves/.migrated ]" 2>/dev/null; then
  # files copied by adb belong to the shell user, so make them readable and writable for the app
  $ADB shell "cp $OLD/* $DEST/../saves/ 2>/dev/null; cp $OLD/../last_game.txt $DEST/../ 2>/dev/null; chmod 666 $DEST/../saves/* $DEST/../last_game.txt 2>/dev/null; touch $DEST/../saves/.migrated"
  echo "copied saves from the old SNES3D install"
fi
echo "copying ROMs from $ROMS ..."
$ADB push --sync "$ROMS"/*.sfc $DEST/ | tail -1
$ADB shell am start -n $PKG/android.app.NativeActivity
echo "started; logs: $ADB logcat -s PopUp16 snes9x OpenXR"
