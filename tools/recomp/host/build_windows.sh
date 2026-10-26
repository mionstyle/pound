#!/usr/bin/env bash
# Cross-compiles the host runtime and recompiled modules into a Windows .exe
# using llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
#
#   build_windows.sh <gen dir> <out dir> [modules...]      (default modules: rtld sdk)
#
# <gen dir> holds the C produced by build_modules.sh (gen/<module>/*.c). The C is
# target independent; only the compiler changes.
# Environment: MINGW (llvm-mingw root), UNICORN_WIN (Unicorn cross build dir),
#              OPT (default -O1), JOBS (default all cores).
set -euo pipefail
GEN=$1
OUT=$2
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

LIBS=()
for m in "${MODULES[@]}"; do
    lib="$OUT/obj/librecomp_$m.a"
    if [ ! -f "$lib" ]; then
        echo "$m: compiling $(ls "$GEN/$m"/*.c | wc -l) files for Windows ($OPT)"
        mkdir -p "$OUT/obj/$m"
        # Objects already built are kept, so an interrupted build resumes where it stopped.
        (cd "$GEN/$m" && ls *.c | xargs -P "$JOBS" -I{} sh -c \
            "[ -s $OUT/obj/$m/{}.o ] || $CC $OPT -fno-strict-aliasing -w -I$ROOT/tools/recomp/runtime -c {} -o $OUT/obj/$m/{}.o.tmp && mv $OUT/obj/$m/{}.o.tmp $OUT/obj/$m/{}.o || echo 'COMPILE FAILED: $m/{}'")
        "$MINGW/bin/llvm-ar" rcs "$lib" "$OUT/obj/$m"/*.o
    fi
    LIBS+=("$lib")
done

echo "linking recomp_run.exe with: ${MODULES[*]}"
"$CC" -O1 -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function \
    -I"$ROOT/tools/recomp/runtime" -I"$ROOT/src/loader" -I"$ROOT/src/core" -I"$UNICORN_SRC/include" \
    "$ROOT"/tools/recomp/host/*.c "$ROOT/src/loader/nso.c" "$ROOT/src/loader/sha256.c" \
    -Wl,--whole-archive "${LIBS[@]}" -Wl,--no-whole-archive \
    "$UNICORN_WIN/libunicorn.dll.a" -static -lpthread -lgdi32 -luser32 -Wl,--stack,67108864 -s \
    -o "$OUT/recomp_run.exe"
cp -f "$UNICORN_WIN/libunicorn.dll" "$OUT/"
cp -f "$MINGW/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$OUT/" # needed by libunicorn.dll
ls -la "$OUT"/*.exe "$OUT"/*.dll
