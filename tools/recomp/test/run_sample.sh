#!/usr/bin/env bash
# Recompile a list of functions (plus callees), build the test library and run
# the differential test against Unicorn.
#
#   run_sample.sh <module.bin> <func list> <work dir> [trials]
set -euo pipefail
MODULE=$1
LIST=$2
WORK=$3
TRIALS=${4:-6}
R=$(cd "$(dirname "$0")/.." && pwd)
JOBS=$(nproc)

rm -rf "$WORK"
python3 "$R/recomp.py" "$MODULE" --out "$WORK" --funcs "$(paste -sd, "$LIST")" \
    --callee-depth 3 --per-file 100 --stats > "$WORK.stats" 2>&1
head -1 "$WORK.stats"

CFLAGS="-O1 -fPIC -fno-strict-aliasing -w -include $R/test/test_config.h -I$R/runtime"
(cd "$WORK" && ls *.c | xargs -P "$JOBS" -I{} sh -c "timeout 600 clang $CFLAGS -c {} -o {}.o || echo 'COMPILE FAILED: {}'")
clang $CFLAGS -c "$R/test/harness.c" -o "$WORK/harness.o"
clang -shared "$WORK"/*.o -o "$WORK/libtest.so" -lm

python3 -I "$R/test/difftest.py" "$MODULE" "$WORK/libtest.so" "$LIST" --trials "$TRIALS" --show 40
