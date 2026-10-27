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

## Status (2026-10-27)

1. Boot: rtld/sdk/main load and relocate; Switch 2 GetInfo ids, sm/TIPC, am (incl. Switch 2
   proxy objects 40/60, graphics supervisor, graphics scaling params), fs (RomFS; save data in
   a host dir, `savefs.c`), acc, time, pctl, aoc, bcat (no cache), nifm (offline), hid
   (vibration list), aud (renderer objects), stpl (streamplay), nfp. Cloned IPC sessions share
   their domain. App memory 10 GB (`APP_MEMORY_SIZE`, DKB heap budgets need > 7 GB).
2. Graphics: `nv::InitializeGraphics` skipped; `nn::vi` on the host; NVN2 procs served by
   `hle_nvn.c` (device info table ids 0..97 in NVN1 order — DKB needs API 231.30; pools,
   buffers, textures with per-format storage sizes, command lists recorded and run at submit:
   render targets + clears; draws only counted). Win32 window / PPM dumps of presented frames
   (black: nothing is drawn yet).
3. Audio: `RequestUpdateAudioRenderer` answered on the host (no sound).
4. Input: Npad No1 is a connected Pro Controller (`hle_hid.c`, `input.c`: keyboard + XInput on
   Windows; `RECOMP_AUTO_PRESS` for headless), six-axis at rest.
5. Progress (cloud, headless): title menu runs, then with auto-press the first level
   `GroundLevelStage` loads and the opening demo (`DemoOpeningDonkeyAppear`) runs for 4000+
   frames without crashing (~14 fps in the 2-core cloud container, main compiled at -O0).
6. Shaders: `RECOMP_SHADER_DUMP=<dir>` captures every NVN2 program (2604 by the title menu).
   `tools/recomp/sm86/` (see its README) translates SM86 code to Vulkan GLSL; 2247/2604 compile
   with glslang. Findings: 0x80-byte header (tag 0x81 vertex / 0x82 fragment, word 0x48 =
   fragment output mask), fragment outputs in R0..R3 per render target at EXIT; textures via
   cbuf handles (bank 2, e.g. c[2][0x20]); driver constants in bank 3 (viewport scale) and
   bank 0 (0x144: 64-bit global memory base used by LDG with UR4); Maxwell attribute space
   (0x70 position, 0x80+16*i generic, 0x2fc vertex id, 0x3fc front facing).
7. Windows: `build_windows.sh <gen> <out> rtld sdk main` builds a 347 MB `recomp_run.exe`
   (zip ~76 MB, sent to the owner as `recomp-win.zip`, not yet tested on Windows).

## Plan (agreed 2026-10-27): move the work to the owner's PC

The renderer needs a real GPU, so development moves to the owner's Windows PC through the
desktop app's device shell (when `device_bash` is available), keeping the cloud for GitHub,
the translator and GPU-less work.
1. On the PC: install llvm-mingw (ucrt, x86_64) and Python 3 (+ `pip install capstone`);
   clone/copy the repo (a mirror lives in `C:\Users\leozi\Desktop\pound-recomp`); get
   Unicorn's `libunicorn.dll` (build-win from the cloud, or build it). The bash build scripts
   need Git Bash or a PowerShell port.
2. Regenerate the C on the PC (`nso_info` on rtld/sdk/main, then `recomp.py` per module, as
   `build_modules.sh` does) and build the .exe natively; run it against the real dump (no
   sparse RomFS needed there).
3. Renderer: port LAS Recomp's Vulkan backend (`runtime/nvn/vulkan/*`, `objects.c`,
   `cmdbuf.c`, `tegra.c` — MIT licensed, github.com/peiikai/LASRecomp) onto `hle_nvn.c`;
   use the sm86 translator (ahead-of-time cache keyed by program hash, like the Fire Emblem
   port described by its author: texture handles from a shared pool, rebuilt compute cbuf 0,
   workgroup size at control +0x58, T239 block-linear tile byte order differs from Switch 1,
   DLSS stubbed as a plain upscale). First goal: the title menu drawn.

## Open problems / next steps

1. Translator gaps: I2F sizes, TMML, TLD4, SHFL, F2FP/HADD2, PLOP3 with uniform preds, a
   texture used with two sampler types, compute shaders (32/36 fail).
2. Unknown yet: vertex attribute -> vertex buffer mapping, which cbuf bank holds the game's
   uniform buffers, vertex output -> fragment varying mapping (assumed generic index i).
3. Speed: main at -O1; NEON/SIMD in the CPU translator; `RECOMP_BLOCK_DISPATCH=1` (runs
   compiled functions entered from the interpreter) still hangs after ~100 s — bisect with
   `RECOMP_BLOCK_MOD=m:r`.
4. Audio mixing on the host.

## Working conventions

- Owner tests only Windows builds; build/test in the cloud, ship an .exe only when useful.
- Copy changed sources to the owner's `C:\Users\leozi\Desktop\pound-recomp` (mirror of the
  repo) and commit/push to `recomp-cpu` regularly.
- The owner has weak Wi-Fi for downloads (no WSL/Ubuntu install for now); uploads from
  their PC to the cloud are fast.
