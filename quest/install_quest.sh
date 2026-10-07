#!/bin/zsh
# First-time setup: installs PopUp16 on a USB-connected Quest and adds your games and covers.
#
# Safe to run again at any time: it never deletes anything on the headset and never replaces a
# game, cover or save that is already there (only files that are missing get copied). To just
# update the app, use quest/update_app.sh instead.
#
# usage: install_quest.sh <rom folder> [--migrate-old-saves]
#   <rom folder>          folder of .sfc / .smc files you own (a covers/ subfolder is used if present)
#   --migrate-old-saves   also copy saves from the old SNES3D app (com.createshinns.snes3d), skipping
#                         any save PopUp16 already has
set -e
set -o pipefail
cd "$(dirname "$0")"
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
ROMS=${1:?usage: install_quest.sh <rom folder> [--migrate-old-saves]}
MIGRATE=${2:-}
PKG=com.createshinns.popup16
FILES=/sdcard/Android/data/$PKG/files
APK=build/popup16.apk

[[ -d "$ROMS" ]] || { echo "no such folder: $ROMS"; exit 1; }
[[ -f "$APK" ]] || { echo "no APK at $APK - run quest/build_apk.sh first"; exit 1; }
$ADB get-state >/dev/null 2>&1 || { echo "No headset found: plug in the Quest, put it on, and accept 'Allow USB debugging'."; exit 1; }

$ADB install -r "$APK"

# The app must create its own folders: Android 14 denies it access to folders adb creates. Start it
# once so it does, then check they belong to the app. Nothing is ever deleted to fix ownership.
$ADB shell am start -n $PKG/android.app.NativeActivity >/dev/null
for i in {1..40}; do $ADB shell "[ -d $FILES/roms ] && [ -d $FILES/covers ] && [ -d $FILES/saves ]" && break; sleep 0.5; done
for d in roms covers saves; do
  owner=$($ADB shell "stat -c %U $FILES/$d 2>/dev/null" | tr -d '\r')
  if [[ "$owner" != u0_a* ]]; then
    echo "The $d folder on the headset is not owned by PopUp16 (owner: ${owner:-missing})."
    echo "Nothing was changed. Fix: open PopUp16 once on the headset, or move that folder aside yourself."
    exit 1
  fi
done

# copy only what is missing, so nothing on the headset is ever replaced
push_missing() {  # <local folder> <headset folder> <glob...>
  local src=$1 dst=$2; shift 2
  local have=$($ADB shell "ls $dst 2>/dev/null" | tr -d '\r')
  local copied=0 skipped=0 f name pat
  local -a files=()
  for pat in "$@"; do files+=("$src"/${~pat}(N)); done   # (N): a pattern with no matches is just empty
  for f in "${(u)files[@]}"; do
    name=${f:t}
    if print -r -- "$have" | grep -Fxq -- "$name"; then skipped=$((skipped + 1)); continue; fi
    $ADB push "$f" "$dst/$name" >/dev/null
    copied=$((copied + 1))
  done
  echo "  $copied copied, $skipped already there"
}
echo "games from $ROMS:"
push_missing "$ROMS" $FILES/roms '*.sfc' '*.smc' '*.SFC' '*.SMC'
if [[ -d "$ROMS/covers" ]]; then
  echo "covers:"
  push_missing "$ROMS/covers" $FILES/covers '*.png' '*.jpg' '*.jpeg' '*.webp'
fi

if [[ "$MIGRATE" == "--migrate-old-saves" ]]; then
  OLD=/sdcard/Android/data/com.createshinns.snes3d/files/saves
  echo "saves from the old SNES3D app:"
  have=$($ADB shell "ls $FILES/saves 2>/dev/null" | tr -d '\r')
  copied=0 skipped=0
  for name in $($ADB shell "ls $OLD 2>/dev/null" | tr -d '\r'); do
    [[ "$name" == .* ]] && continue
    if print -r -- "$have" | grep -Fxq -- "$name"; then skipped=$((skipped + 1)); continue; fi
    $ADB shell "cp '$OLD/$name' '$FILES/saves/$name' && chmod 666 '$FILES/saves/$name'"   # adb's copy must stay app-writable
    copied=$((copied + 1))
  done
  echo "  $copied copied, $skipped kept (PopUp16 already had them)"
fi

echo "done. Open PopUp16 on the headset (it is running now). Logs: $ADB logcat -s PopUp16"
