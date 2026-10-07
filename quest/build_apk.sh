#!/bin/zsh
# Builds build/popup16.apk for Meta Quest (arm64) without Gradle: CMake/NDK -> aapt2 -> zipalign -> apksigner.
set -e
cd "$(dirname "$0")"
export JAVA_HOME=/opt/homebrew/opt/openjdk@17 PATH=/opt/homebrew/opt/openjdk@17/bin:$PATH
SDK=$HOME/Library/Android/sdk
NDK=$SDK/ndk/27.2.12479018
BT=$SDK/build-tools/34.0.0
# DEV=1 quest/build_apk.sh builds a test build with the developer hooks compiled in
cmake -S . -B build/cmake -G Ninja -DPOPUP16_DEV_HOOKS=$([[ -n "$DEV" ]] && echo ON || echo OFF) -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/cmake
rm -rf build/apk && mkdir -p build/apk/lib/arm64-v8a
cp build/cmake/libpopup16quest.so third_party/oxr/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so build/apk/lib/arm64-v8a/
$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip build/apk/lib/arm64-v8a/libpopup16quest.so
(cd .. && python3 tools/make_notices.py >/dev/null)   # refresh assets/NOTICES.txt
rm -rf build/res && mkdir -p build/res
$BT/aapt2 compile --dir res -o build/res/res.zip
$BT/aapt2 link -o build/unsigned.apk --manifest AndroidManifest.xml -I $SDK/platforms/android-32/android.jar \
  -R build/res/res.zip -A assets --auto-add-overlay
(cd build/apk && zip -qr ../unsigned.apk lib)
$BT/zipalign -f -p 4 build/unsigned.apk build/aligned.apk
if [[ -n "$RELEASE_KEYSTORE" ]]; then
  # release build: your own key (quest/make_release_key.sh); apksigner asks for its password
  [[ -f "$RELEASE_KEYSTORE" ]] || { echo "no keystore at $RELEASE_KEYSTORE"; exit 1; }
  [[ -z "$DEV" ]] || { echo "refusing to sign a DEV (test-hook) build with the release key"; exit 1; }
  $BT/apksigner sign --ks "$RELEASE_KEYSTORE" --ks-key-alias popup16 --out build/popup16.apk build/aligned.apk
  $BT/apksigner verify --print-certs build/popup16.apk | head -2
else
  # everyday builds: the standard Android debug key (fine for your own headset, not for publishing)
  KS=$HOME/.android/debug.keystore
  [[ -f $KS ]] || { mkdir -p ~/.android; keytool -genkeypair -keystore $KS -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1; }
  $BT/apksigner sign --ks $KS --ks-pass pass:android --key-pass pass:android --out build/popup16.apk build/aligned.apk
fi
echo "built $(pwd)/build/popup16.apk"
