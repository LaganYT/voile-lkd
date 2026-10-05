#!/bin/sh
# Build obelisk-input (native arm64, ad-hoc signed, no entitlements) with SDL3 linked in statically.
# SDL comes from its release archive, checked against the pinned SHA-256, and is built once with only what
# gamepads need (no video, audio, rendering or force feedback). Needs cmake and ninja.
set -eu
S=$(cd "$(dirname "$0")" && pwd)
SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
B="$S/build"
SDL="$B/SDL3-$SDL_VERSION"
PREFIX="$B/sdl3"
mkdir -p "$B" "$S/bin"

if [ ! -f "$PREFIX/lib/libSDL3.a" ] || [ "$(cat "$PREFIX/VERSION" 2>/dev/null)" != "$SDL_VERSION" ]; then
  archive="$B/SDL3-$SDL_VERSION.tar.gz"
  if [ ! -f "$archive" ] || [ "$(shasum -a 256 "$archive" | cut -d' ' -f1)" != "$SDL_SHA256" ]; then
    curl -fL --retry 3 -o "$archive.partial" \
      "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    [ "$(shasum -a 256 "$archive.partial" | cut -d' ' -f1)" = "$SDL_SHA256" ] || { echo "SDL3 checksum mismatch" >&2; exit 1; }
    mv "$archive.partial" "$archive"
  fi
  rm -rf "$SDL" "$B/sdl3-build" "$PREFIX"
  tar -xzf "$archive" -C "$B"
  cmake -S "$SDL" -B "$B/sdl3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_INSTALL_DOCS=OFF -DSDL_AUDIO=OFF -DSDL_VIDEO=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_CAMERA=OFF \
    -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_HAPTIC=OFF -DSDL_JOYSTICK=ON -DSDL_HIDAPI=ON -DSDL_SENSOR=ON \
    -DSDL_POWER=ON >"$B/sdl3-configure.log"
  cmake --build "$B/sdl3-build" -j "${JOBS:-4}" >"$B/sdl3-build.log"
  cmake --install "$B/sdl3-build" >"$B/sdl3-install.log"
  echo "$SDL_VERSION" >"$PREFIX/VERSION"
fi

# The frameworks and libraries SDL's static build names for its users, without the library itself.
sdl_libs=$(sed -n 's/^Libs: *//p' "$PREFIX/lib/pkgconfig/sdl3.pc" | sed -e 's/-L[^ ]*//' -e 's/-lSDL3//')
# shellcheck disable=SC2086
clang -arch arm64 -O2 -Wall -I"$PREFIX/include" -o "$S/bin/obelisk-input" "$S/obelisk-input.c" \
  "$PREFIX/lib/libSDL3.a" $sdl_libs -framework CoreFoundation
codesign -s - -f "$S/bin/obelisk-input"
cp "$SDL/LICENSE.txt" "$S/bin/SDL3-LICENSE.txt"
shasum -a 256 "$S/obelisk-input.c" "$S/obelisk_input_protocol.h" "$S/bin/obelisk-input"
