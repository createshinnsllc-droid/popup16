#!/bin/zsh
# Fetches dependencies for a fresh checkout: patched snes9x core and the Khronos OpenXR loader.
set -e
cd "$(dirname "$0")"
SNES9X_BASE=1bcc369e89f08243e0a462882fb1f3e42e51de3a
if [[ ! -d snes9x ]]; then
  git clone https://github.com/snes9xgit/snes9x.git snes9x
  git -C snes9x checkout -b stereo3d $SNES9X_BASE
  git -C snes9x am ../snes9x-stereo3d.patch
fi
OXR=quest/third_party
if [[ ! -d $OXR/oxr ]]; then
  [[ -f $OXR/openxr_loader_for_android-1.1.63.aar ]] || curl -sL -o $OXR/openxr_loader_for_android-1.1.63.aar \
    https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/openxr_loader_for_android-1.1.63.aar
  mkdir -p $OXR/oxr && unzip -oq $OXR/openxr_loader_for_android-1.1.63.aar -d $OXR/oxr
fi
echo "ready: ./build.sh (Mac app), quest/build_apk.sh (Quest APK)"
