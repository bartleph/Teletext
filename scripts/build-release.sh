#!/usr/bin/env bash
# Build and package one release target.
#
#   scripts/build-release.sh <target> [version]
#
# Targets: linux-amd64  linux-arm64  linux-armv7  linux-armv6  windows-amd64  macos-arm64
#
# The Linux and Windows targets are cross-compiled on an x86-64 Linux machine
# (the release workflow does this on ubuntu-latest); macos-arm64 is built natively
# on a Mac. You need these compilers on the build machine:
#   linux-amd64    g++
#   linux-arm64    g++-aarch64-linux-gnu
#   linux-armv7    g++-arm-linux-gnueabihf
#   linux-armv6    the dockcross/linux-armv6 image (it sets $CXX): Raspberry Pi 1 and Zero need
#                  ARMv6 code AND libraries, which Ubuntu's ARM compilers don't make.
#                  docker run --rm -v "$PWD:/work" -w /work dockcross/linux-armv6:<tag> \
#                      bash scripts/build-release.sh linux-armv6 <version>
#   windows-amd64  g++-mingw-w64-x86-64
#   macos-arm64    clang++ (Xcode command line tools)
#
# Output: dist/teletext-<version>-<target>.tar.gz  (.zip for Windows)
# The archive unpacks to a folder laid out like the repository (server/, feed/, ...), so
# the settings files' "pagesdir=../server/pages" work straight away.
set -euo pipefail

TARGET="${1:?usage: $0 <target> [version]}"
VERSION="${2:-dev}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="teletext-${VERSION}-${TARGET}"
OUT="$ROOT/dist/$NAME"

EXT=""
STATIC=""
case "$TARGET" in
  linux-amd64)   CXX=g++;                         STATIC="-static" ;;
  linux-arm64)   CXX=aarch64-linux-gnu-g++;       STATIC="-static" ;;
  linux-armv7)   CXX=arm-linux-gnueabihf-g++;     STATIC="-static" ;;
  linux-armv6)   CXX="${CXX:?linux-armv6 must be built inside the dockcross/linux-armv6 image}"; STATIC="-static" ;;
  windows-amd64) CXX=x86_64-w64-mingw32-g++;      STATIC="-static -static-libgcc -static-libstdc++"; EXT=".exe" ;;
  macos-arm64)   CXX=clang++;                     STATIC="-mmacosx-version-min=11.0" ;;
  *) echo "unknown target: $TARGET" >&2; exit 1 ;;
esac
WINDOWS=0; [ "$TARGET" = "windows-amd64" ] && WINDOWS=1
STRIP="-s"; [ "$TARGET" = "macos-arm64" ] && STRIP=""        # Apple's linker doesn't take -s

# build <folder> <program> <windows linker flags>
# The Windows-only GUI programs (client, editor) are skipped on the other systems.
build() {
  local dir="$1" prog="$2" winlibs="$3"
  mkdir -p "$OUT/$dir"
  echo "== $prog"
  if [ "$WINDOWS" = 1 ]; then
    # shellcheck disable=SC2086
    "$CXX" -O2 $STATIC $STRIP -o "$OUT/$dir/$prog$EXT" "$ROOT/$dir/$prog.cpp" $winlibs
  else
    # shellcheck disable=SC2086
    "$CXX" -O2 $STATIC $STRIP -o "$OUT/$dir/$prog" "$ROOT/$dir/$prog.cpp"
  fi
}

rm -rf "${OUT:?}" "${OUT:?}.tar.gz" "${OUT:?}.zip"
mkdir -p "$OUT"

build server  ttxserver  "-lws2_32"
build feed    ttxfeed    "-lwininet"
build flights ttxflights "-lwininet"
build weather ttxweather "-lwininet"
build tv      ttxtv      "-lwininet"
if [ "$WINDOWS" = 1 ]; then
  build client ttxclient "-mwindows -lws2_32 -lgdi32"
  build editor ttxedit   "-mwindows -lws2_32 -lgdi32 -lcomdlg32"
fi

# settings, demo pages, the browser client and the README
cp "$ROOT/README.md" "$OUT/"
cp "$ROOT/server/ttxserver.cfg" "$OUT/server/"
cp -R "$ROOT/server/web" "$OUT/server/web"
cp -R "$ROOT/server/pages" "$OUT/server/pages"
cp "$ROOT/feed/ttxfeed.cfg" "$OUT/feed/"
cp "$ROOT/flights/"*.cfg "$OUT/flights/"
cp "$ROOT/weather/ttxweather.cfg" "$OUT/weather/"
cp "$ROOT/tv/ttxtv.cfg" "$OUT/tv/"
if [ "$WINDOWS" = 1 ]; then
  cp "$ROOT/client/ttxclient.cfg" "$OUT/client/"
  cp "$ROOT/editor/ttxedit.cfg" "$OUT/editor/"
else
  # ttxfeed.cfg ships with Windows-style slashes in pagesdir
  sed -i.bak '/^pagesdir=/ s#\\#/#g' "$OUT/feed/ttxfeed.cfg" && rm -f "$OUT/feed/ttxfeed.cfg.bak"
fi

cat > "$OUT/START-HERE.txt" <<EOF
Teletext $VERSION ($TARGET)

Start the server:   cd server && ./ttxserver$EXT
Then open http://localhost:8080 in a browser, or run the feed programs (feed, weather,
tv, flights) next to it. Each program reads the settings file beside it.
See README.md for everything else.
EOF
if [ "$WINDOWS" = 1 ]; then
  sed -i 's#\./ttxserver#ttxserver#; s#cd server &&#cd server, then run#' "$OUT/START-HERE.txt"
  printf '\nThe Windows client (client\\ttxclient.exe) and the page editor (editor\\ttxedit.exe) are included.\n' >> "$OUT/START-HERE.txt"
else
  printf '\nThe Windows client and page editor are Windows-only and are not in this package.\n' >> "$OUT/START-HERE.txt"
fi

cd "$ROOT/dist"
if [ "$WINDOWS" = 1 ]; then
  zip -qr "$NAME.zip" "$NAME"
  echo "built dist/$NAME.zip"
else
  tar -czf "$NAME.tar.gz" "$NAME"
  echo "built dist/$NAME.tar.gz"
fi
