#!/usr/bin/env bash
# One-shot setup + build + run inside WSL (Ubuntu 24.04) or any Linux machine.
#
#   bash setup_wsl.sh <exefs dir>
#
# Example (WSL, Windows paths live under /mnt/c):
#   bash setup_wsl.sh "/mnt/c/Users/leozi/Downloads/DonkeyKongBananza/DonkeyKongBananza/updates/458752/exefs"
#
# Everything is built under $WORK (default ~/recomp), on the Linux filesystem:
# building on /mnt/c is several times slower.
# Environment: OPT=-O0 for a fast development build (default -O1), JOBS=<n> (default all cores).
set -euo pipefail
EXEFS=${1:?usage: setup_wsl.sh <exefs dir with main, sdk, rtld, main.npdm>}
WORK=${WORK:-$HOME/recomp}
OPT=${OPT:--O1}
JOBS=${JOBS:-$(nproc)}
step() { printf '\n==== %s\n' "$*"; }

step "packages"
if ! command -v clang >/dev/null || ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
    sudo apt-get update
    sudo apt-get install -y git clang cmake ninja-build python3 build-essential
fi

mkdir -p "$WORK/deps" "$WORK/mods" "$WORK/exefs"
cd "$WORK"

step "sources"
[ -d pound ] || git clone -b recomp-cpu https://github.com/mionstyle/pound.git
git -C pound pull --ff-only || true
[ -d deps/capstone ] || git clone --depth 1 -b 5.0.6 https://github.com/capstone-engine/capstone.git deps/capstone
[ -d deps/unicorn ] || git clone --depth 1 -b 2.1.3 https://github.com/unicorn-engine/unicorn.git deps/unicorn

step "capstone (ARM64 decoder)"
if [ ! -f deps/capstone/build/libcapstone.so ]; then
    cmake -S deps/capstone -B deps/capstone/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON -DBUILD_SHARED_LIBS=ON \
        -DCAPSTONE_BUILD_TESTS=OFF >/dev/null
    cmake --build deps/capstone/build
fi

step "unicorn (fallback interpreter)"
if [ ! -f deps/unicorn/build/libunicorn.so ]; then
    cmake -S deps/unicorn -B deps/unicorn/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF >/dev/null
    cmake --build deps/unicorn/build
fi

export LIBCAPSTONE_PATH="$WORK/deps/capstone/build"
export CAPSTONE_PY="$WORK/deps/capstone/bindings/python"
export UNICORN_DIR="$WORK/deps/unicorn"
R="$WORK/pound/tools/recomp"

step "copy exefs to the Linux filesystem"
cp -f "$EXEFS"/main "$EXEFS"/sdk "$EXEFS"/rtld "$EXEFS"/main.npdm exefs/

step "decompress NSOs"
clang -O2 -I pound/src/core -I pound/src/loader pound/src/loader/nso.c pound/src/loader/sha256.c \
    pound/tools/nso_info.c -o nso_info
for m in rtld sdk main; do
    ./nso_info "exefs/$m" "mods/$m.bin" | tail -1
done

step "translate + compile ($JOBS jobs, $OPT)"
start=$(date +%s)
CFLAGS="$OPT" JOBS="$JOBS" "$R/host/build_modules.sh" mods gen
echo "modules built in $(( $(date +%s) - start )) s"

step "link the host runtime"
"$R/host/build_host.sh" gen recomp_run

step "boot"
./recomp_run exefs 2>&1 | tee boot.log | tail -40
echo
echo "full log: $WORK/boot.log   (rerun: cd $WORK && ./recomp_run exefs, add -v for SVC tracing)"
