# Handoff: Donkey Kong Bananza static recompilation

Read this first if you are a new assistant (or developer) picking up this project.
Repository: `mionstyle/pound`, branch **`recomp-cpu`**. Everything below is in `tools/recomp/`
unless stated otherwise. Talk to the owner in **Portuguese (pt-PT)**; write all code,
comments and docs in **English**.

## Goal

Run the owner's own dump of *Donkey Kong Bananza* (Switch 2, title 040074A01BF12000,
update 458752) on a Windows PC by **static recompilation**: AArch64 machine code is
translated to C ahead of time; the OS (Horizon), services and the GPU (NVN2) are provided
by a host runtime (high-level emulation, like the Switch 1 "LAS Recomp" project).

## Where things are

| Path | What |
| --- | --- |
| `src/loader/nso.c`, `tools/nso_info.c` | NSO loader / decompressor (`nso_info <nso> out.bin`) |
| `tools/recomp/analyze.py` | module parser (MOD0, symbols, relocs incl. RELR, .eh_frame functions) |
| `tools/recomp/recomp.py` | the translator (AArch64 -> C). Options: `--prefix --per-file --stats --funcs --callee-depth --no-comments` |
| `tools/recomp/runtime/recomp_cpu.h` | `cpu_t` and helpers used by generated code (append new fields at the end only) |
| `tools/recomp/test/` | differential tests vs Unicorn (`difftest.py`) |
| `tools/recomp/host/` | the runtime: `memory.c loader.c exec.c kernel.c ipc.c romfs.c savefs.c hle*.c present.c input.c main.c` |
| `tools/recomp/host/build_modules.sh` | translate + compile rtld/sdk/main into `librecomp_<m>.a` |
| `tools/recomp/host/build_host.sh` | link Linux test binary |
| `tools/recomp/host/build_windows.sh` | cross-compile Windows .exe with llvm-mingw (resumable) |
| `tools/recomp/pack-romfs.ps1` | (owner's PC) split RomFS folders into zips < 400 MB |
| `docs/recomp/dkb-analysis.md`, `dkb-nvn2-procs.txt` | analysis notes, the 726 NVN2 procs the game requests |

## Building (Linux, e.g. a cloud container)

Dependencies are built from GitHub source (PyPI/apt may be blocked): Capstone 5.0.6 and
Unicorn 2.1.3 (`cmake` builds; Unicorn also cross-built for Windows into `build-win`),
llvm-mingw 20240619 (ucrt) for Windows. Environment used:
`LIBCAPSTONE_PATH=<capstone>/build`, Capstone python bindings on `CAPSTONE_PY`.

```
nso_info <exefs>/main main.bin   (same for sdk, rtld)
host/build_modules.sh <dir with rtld.bin sdk.bin main.bin> gen/     # main: CFLAGS=-O0 to compile faster
host/build_host.sh gen/ rr_full
RECOMP_SAVE_DIR=save ./rr_full [-v|-vv] <exefs dir> <romfs dir>
OPT=-O0 host/build_windows.sh gen/ win/ rtld sdk main               # ~hours on 2 cores
```

Useful environment variables: `RECOMP_SAVE_DIR` (save data dir), `RECOMP_AUTO_PRESS=<n>`
(tap A every n polls, headless), `RECOMP_DUMP_FRAMES=<dir>` (PPM of presented frames),
`RECOMP_PROFILE=1` (interpreter profile every 300 frames), `RECOMP_NVN_TRACE=1`,
`RECOMP_IPC_TRACE=1`, `RECOMP_BLOCK_DISPATCH=1` (experimental, see open problems),
`RECOMP_WATCH=main+0x...` (dump words at Break). `-v` logs RomFS reads / IPC; `-vv` every SVC.

## Game data

The dump lives on the owner's PC: `C:\Users\leozi\Downloads\DonkeyKongBananza\DonkeyKongBananza`
(`base`, `updates\458752` with `exefs` and a complete patched `romfs`, `dlc`).
`romfs-lista.txt` there lists every RomFS file with its size. In the cloud we keep a
**sparse mirror**: every file exists, real content only for files fetched so far (zero
placeholders otherwise); missing files show up as `LoadResult = -1` in the game log or as
reads of placeholder files. About 2.3 GB of real data was needed so far (System, SystemData,
Pack, Model, Tex, Sound minus Resource, Layout, ObjectData, Voxel minus Const, ...).
Files are fetched through the desktop bridge (`device_stage_files`, 400 MB per file) or
as zips the owner creates with `pack-romfs.ps1`.

## Status (2026-10-26)

1. Boot: rtld/sdk/main load and relocate; Switch 2 GetInfo ids, sm/TIPC, am (incl. Switch 2
   proxy objects 40/60, graphics supervisor), fs (RomFS, save data on a host dir), acc,
   time, pctl, aoc, bcat (no cache), nifm (offline), hid (vibration list), aud (renderer
   objects), stpl (streamplay), nfp.
2. Graphics: `nv::InitializeGraphics` skipped; `nn::vi` on the host; NVN2 procs served by
   `hle_nvn.c` (device info table, pools, buffers, textures, command lists recorded and run
   at submit: render targets + clears; draws only counted). Window/PPM output of the
   presented texture (black so far: the game only clears it and blits).
3. Audio: `RequestUpdateAudioRenderer` answered on the host (no sound yet).
4. Input: Npad No1 is a connected Pro Controller (`hle_hid.c` + `input.c`: keyboard and
   XInput on Windows), six-axis at rest, ShowControllerSupport succeeds.
5. Progress: title menu runs (frames presented); with `RECOMP_AUTO_PRESS` the game starts
   loading the first level `GroundLevelStageScenario01` and then needs Phive, Voxel/Const,
   Effect, ClusteredRendering (owner was asked to pack them with `pack-romfs.ps1 ... -Name lv`).

## Open problems / next steps

1. Fetch the level data (`romfs-lv*.zip`), fix what breaks until the level runs.
2. Windows `.exe` with main compiled (`build_windows.sh ... rtld sdk main`), test on the PC.
3. Renderer (big): Vulkan backend; translate NVN2 shaders (SM86-class) to SPIR-V; start
   with 2D/UI, then 3D. LAS Recomp and oboromi are references.
4. Speed: compile main with -O1; translate NEON/SIMD (most remaining interpreter use);
   fix the hang with `RECOMP_BLOCK_DISPATCH=1` (runs compiled functions entered from the
   interpreter; it once ran a replaced function's real code, fixed by skipping HLE
   addresses; a hang after ~100 s remains — bisect with `RECOMP_BLOCK_MOD=m:r`).
5. Audio mixing on the host; real hid shared memory if needed.

## Working conventions

- Owner tests only Windows builds; build/test in the cloud, ship an .exe only when useful.
- Copy changed sources to the owner's `C:\Users\leozi\Desktop\pound-recomp` (mirror of the
  repo) and commit/push to `recomp-cpu` regularly.
- The owner has weak Wi-Fi for downloads (no WSL/Ubuntu install for now); uploads from
  their PC to the cloud are fast.
