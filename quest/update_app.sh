#!/bin/zsh
# App-only update: replaces the PopUp16 app with a new build, and nothing else.
#
# This never deletes, copies or overwrites games, covers, saves, settings, controls or library data,
# and never launches the app. Use this to try a new build. quest/install_quest.sh is the separate
# first-time provisioning script and is not part of an update.
#
# usage: update_app.sh [path/to/popup16.apk]
set -e
cd "$(dirname "$0")"
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
APK=${1:-build/popup16.apk}
PKG=com.createshinns.popup16

[[ -f "$APK" ]] || { echo "no APK at $APK - run quest/build_apk.sh first"; exit 1; }
$ADB get-state >/dev/null 2>&1 || {
  echo "No headset found: plug in the Quest, put it on, and accept 'Allow USB debugging'."
  exit 1
}

# Record what is there before touching anything, so the result can be compared afterwards.
echo "before: $($ADB shell pm list packages | grep -c "^package:$PKG$") install(s) of $PKG present"
$ADB shell "ls /sdcard/Android/data/$PKG/files 2>/dev/null" | tr -d '\r' | sed 's/^/  data: /'

# -r re-installs in place, keeping the app's data. No -d, no uninstall, no adb shell data commands.
$ADB install -r "$APK"

echo "updated in place. Your games, covers, saves, settings and library were not touched."
echo "If the new build behaves badly, reinstall the previous APK the same way; no data is removed."
