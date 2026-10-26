#!/usr/bin/env bash
# Builds the host runtime, linking whichever recompiled modules exist.
#
#   build_host.sh <gen dir with librecomp_*.a> <output binary>
# Environment: UNICORN_DIR (default /home/claude/deps/unicorn)
set -euo pipefail
GEN=$1
OUT=$2
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
UNICORN_DIR=${UNICORN_DIR:-/home/claude/deps/unicorn}
LIBS=()
for m in rtld sdk main; do
    [ -f "$GEN/librecomp_$m.a" ] && LIBS+=("$GEN/librecomp_$m.a")
done
echo "linking recompiled modules: ${LIBS[*]:-none}"
clang -O1 -g -rdynamic -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=gnu11 \
    -I"$ROOT/tools/recomp/runtime" -I"$ROOT/src/loader" -I"$ROOT/src/core" -I"$UNICORN_DIR/include" \
    "$ROOT"/tools/recomp/host/*.c "$ROOT/src/loader/nso.c" "$ROOT/src/loader/sha256.c" \
    -Wl,--whole-archive "${LIBS[@]}" -Wl,--no-whole-archive \
    -L"$UNICORN_DIR/build" -lunicorn -Wl,-rpath,"$UNICORN_DIR/build" -lm -lpthread -o "$OUT"
