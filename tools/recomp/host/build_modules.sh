#!/usr/bin/env bash
# Recompile the exefs modules of a title into static libraries.
#
#   build_modules.sh <dir with rtld.bin sdk.bin main.bin> <out dir>
#
# Each module gets its own symbol prefix (f_rtld_*, f_sdk_*, f_main_*). Instructions the
# translator does not support yet hand the call over to the fallback interpreter.
set -euo pipefail
IN=$1
OUT=$2
R=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-$(nproc)}
mkdir -p "$OUT"
CFLAGS="${CFLAGS:--O1} -fPIC -fno-strict-aliasing -w -I$R/runtime"

for m in rtld sdk main; do
    [ -f "$OUT/librecomp_$m.a" ] && { echo "$m: already built"; continue; }
    echo "$m: translating"
    rm -rf "$OUT/$m"
    python3 "$R/recomp.py" "$IN/$m.bin" --out "$OUT/$m" --prefix "$m" --no-comments \
        --per-file 400 --stats > "$OUT/$m.stats" 2>&1
    head -1 "$OUT/$m.stats"
    echo "$m: compiling $(ls "$OUT/$m"/*.c | wc -l) files"
    (cd "$OUT/$m" && ls *.c | xargs -P "$JOBS" -I{} sh -c "clang $CFLAGS -c {} -o {}.o || echo 'COMPILE FAILED: $m/{}'")
    ar rcs "$OUT/librecomp_$m.a" "$OUT/$m"/*.o
    echo "$m: done"
done
