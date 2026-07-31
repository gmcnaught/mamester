#!/usr/bin/env bash
# Build a LEAN static SDL 1.2 for MiSTer armhf, static-linked into the mame bench
# binary so it needs no libSDL.so at runtime.
#
# Debian's libSDL-1.2 DT_NEEDEDs X11/Xext/pulse/caca/dbus — none present on the
# MiSTer rootfs (only glib/alsa/libstdc++ are). This keeps ONLY what mame4all
# touches: the dummy video driver (SDL_VIDEODRIVER=dummy at runtime, for
# SDL_SetVideoMode) and the Linux evdev/joystick subsystem. Audio is cut entirely
# (mame does its own ALSA; init_SDL passes SDL_INIT_JOYSTICK only). Mirrors the
# approach in solarus-mister/scripts/build_sdl2.sh, for SDL 1.2 instead of 2.
#
# Runs INSIDE the armhf container with cwd=/src (the mame4all-pi root). Native
# build (gcc is armhf under qemu), so no --host. Idempotent: skips if libSDL.a
# already exists.
set -euo pipefail

VER="${SDL12_VER:-1.2.15}"
SRC="work/SDL-$VER"
PREFIX="/src/work/sdl12-prefix"

if [ -f "$PREFIX/lib/libSDL.a" ]; then
    echo "# libSDL.a present, skipping SDL build ($PREFIX/lib/libSDL.a)"
else
    [ -f "work/SDL-$VER.tar.gz" ] || { echo "missing work/SDL-$VER.tar.gz"; exit 1; }
    [ -d "$SRC" ] || tar -C work -xzf "work/SDL-$VER.tar.gz"
    (
        cd "$SRC"
        export CFLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -O3"
        ./configure --prefix="$PREFIX" \
            --enable-static --disable-shared \
            --disable-audio \
            --enable-video-dummy \
            --disable-video-x11 --disable-video-directfb --disable-video-fbcon \
            --disable-video-dga --disable-video-svga --disable-video-aalib \
            --disable-video-caca --disable-video-opengl --disable-video-ggi \
            --disable-input-tslib --enable-joystick \
            --disable-pulseaudio --disable-esd --disable-arts --disable-nas \
            --disable-nasm --disable-sdl-dlopen
        make -j"$(nproc)"
        make install
    )
fi

echo "=== libSDL.a ==="; ls -la "$PREFIX/lib/libSDL.a"
