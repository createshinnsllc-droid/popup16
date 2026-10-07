#!/bin/zsh
# Creates YOUR release signing key for PopUp16. Run it once, yourself: keytool asks you to choose a
# password, which never leaves this Mac. Every public update must be signed with this same key, so
# back up the file and remember the password (a lost key means users must uninstall to update).
#
# usage: quest/make_release_key.sh [path]   (default ~/.android/popup16-release.jks)
# then:  RELEASE_KEYSTORE=~/.android/popup16-release.jks quest/build_apk.sh
set -e
export JAVA_HOME=/opt/homebrew/opt/openjdk@17 PATH=/opt/homebrew/opt/openjdk@17/bin:$PATH
KS=${1:-$HOME/.android/popup16-release.jks}
[[ -e "$KS" ]] && { echo "$KS already exists - not overwriting your key"; exit 1; }
mkdir -p "$(dirname "$KS")"
keytool -genkeypair -keystore "$KS" -alias popup16 -keyalg RSA -keysize 4096 -validity 36500 \
  -dname "CN=PopUp16, O=CreateShinns LLC"
chmod 600 "$KS"
echo "created $KS - back it up somewhere safe (not in the git repo)"
