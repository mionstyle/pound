#!/usr/bin/env bash
# Compiles the host runtime and recompiled modules into a Windows .exe with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw): cross-compiled on Linux, or natively on Windows
# from Git Bash. The Vulkan renderer (host/vk/) needs the Vulkan headers and vulkan-1.lib
# from the LunarG Vulkan SDK ($VULKAN_SDK, set by its installer).
#
#   build_windows.sh <gen dir> <out dir> [modules...]      (default modules: rtld sdk)
#
# <gen dir> holds the C produced by build_modules.sh (gen/<module>/*.c). The C is
# target independent; only the compiler changes.
# Environment: MINGW (llvm-mingw root), UNICORN_SRC (Unicorn source, for its headers),
#              UNICORN_WIN (dir with libunicorn.dll and libunicorn.dll.a), VULKAN_SDK
#              (empty: no renderer), OPT (default -O1), JOBS (default all cores).
set -euo pipefail
GEN=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
shift 2
MODULES=("${@:-rtld sdk}")
[ $# -eq 0 ] && MODULES=(rtld sdk)
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MINGW=${MINGW:-/home/claude/deps/mingw/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64}
UNICORN_SRC=${UNICORN_SRC:-/home/claude/deps/unicorn}
UNICORN_WIN=${UNICORN_WIN:-$UNICORN_SRC/build-win}
OPT=${OPT:--O1}
JOBS=${JOBS:-$(nproc)}
CC="$MINGW/bin/x86_64-w64-mingw32-clang"
mkdir -p "$OUT/obj"

VK_FLAGS=()
VK_SRC=()
if [ -n "${VULKAN_SDK:-}" ]; then
    VK=$(cd "$VULKAN_SDK" && pwd)
    VK_FLAGS=(-DRECOMP_VULKAN -I"$VK/Include")
    VK_SRC=("$ROOT"/tools/recomp/host/vk/*.c "$VK/Lib/vulkan-1.lib")
fi

LIBS=()
for m in "${MODULES[@]}"; do
    lib="$OUT/obj/librecomp_$m.a"
    if [ ! -f "$lib" ]; then
        echo "$m: compiling $(ls "$GEN/$m"/*.c | wc -l) files for Windows ($OPT)"
        mkdir -p "$OUT/obj/$m"
        # Objects already built are kept, so an interrupted build resumes where it stopped.
        export CC OPT OUT ROOT m
        (cd "$GEN/$m" && ls *.c | xargs -P "$JOBS" -I{} sh -c \
            '[ -s "$OUT/obj/$m/{}.o" ] || { "$CC" $OPT -fno-strict-aliasing -w -I"$ROOT/tools/recomp/runtime" -c {} -o "$OUT/obj/$m/{}.o.tmp" && mv "$OUT/obj/$m/{}.o.tmp" "$OUT/obj/$m/{}.o"; } || echo "COMPILE FAILED: $m/{}"')
        "$MINGW/bin/llvm-ar" rcs "$lib" "$OUT/obj/$m"/*.o
    fi
    LIBS+=("$lib")
done

echo "linking recomp_run.exe with: ${MODULES[*]}${VK_SRC:+ (Vulkan renderer)}"
"$CC" -O1 -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function \
    -I"$ROOT/tools/recomp/runtime" -I"$ROOT/src/loader" -I"$ROOT/src/core" -I"$UNICORN_SRC/include" \
    "${VK_FLAGS[@]}" "$ROOT"/tools/recomp/host/*.c "${VK_SRC[@]}" "$ROOT/src/loader/nso.c" "$ROOT/src/loader/sha256.c" \
    -Wl,--whole-archive "${LIBS[@]}" -Wl,--no-whole-archive \
    "$UNICORN_WIN/libunicorn.dll.a" -static -lpthread -lgdi32 -luser32 -Wl,--stack,67108864 -s \
    -o "$OUT/recomp_run.exe"
cp -f "$UNICORN_WIN/libunicorn.dll" "$OUT/"
cp -f "$MINGW/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$OUT/" # needed by libunicorn.dll
ls -la "$OUT"/*.exe "$OUT"/*.dll
