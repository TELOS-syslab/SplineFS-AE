#!/bin/bash
# make_apks.sh [DIR] -- download open-source Android APKs from F-Droid and
# extract each into a per-app tree of assets, layouts and strings (default
# DIR: datasets/build/apks).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MASTER=${1:-$HERE/build/apks}
EXTRACT=$MASTER/extracted
mkdir -p "$MASTER" "$EXTRACT"
# F-Droid package ids; each package page gives the current APK link.
PKGS=(
  org.fdroid.fdroid org.telegram.messenger info.guardianproject.orbot
  com.nextcloud.client org.videolan.vlc org.mozilla.fennec_fdroid
  com.fsck.k9 org.schabi.newpipe net.osmand.plus org.wikipedia
  com.termux org.kde.kdeconnect_tp im.vector.app org.thoughtcrime.securesms
  de.danoeh.antennapod org.fossify.gallery org.fossify.messages
  com.simplemobiletools.notes.pro org.documentfoundation.libreoffice ws.xsoh.etar
)
for pkg in "${PKGS[@]}"; do
  [ -d "$EXTRACT/$pkg" ] && { echo "  have $pkg"; continue; }
  page="https://f-droid.org/en/packages/$pkg/"
  url=$(curl -sL "$page" | grep -oE 'https://f-droid.org/repo/[^"]+\.apk' | head -1)
  [ -z "$url" ] && { echo "  no apk url: $pkg"; continue; }
  apk="$MASTER/$pkg.apk"
  curl -sL "$url" -o "$apk" || { echo "  dl fail $pkg"; continue; }
  mkdir -p "$EXTRACT/$pkg"
  unzip -qq -o "$apk" -d "$EXTRACT/$pkg" 2>/dev/null || true
  echo "  $pkg: $(find "$EXTRACT/$pkg" -type f | wc -l) files"
done
echo "apk asset tree: $EXTRACT"
echo "  apps:  $(find "$EXTRACT" -maxdepth 1 -mindepth 1 -type d | wc -l)"
echo "  files: $(find "$EXTRACT" -type f | wc -l)"
echo "  dirs:  $(find "$EXTRACT" -type d | wc -l)"
( cd "$EXTRACT" && tar -cf "$MASTER/../apks_assets.tar" . ) && \
  echo "staged tar: $(du -h "$MASTER/../apks_assets.tar"|cut -f1)"
