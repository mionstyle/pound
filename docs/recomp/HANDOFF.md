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
| `tools/recomp/host/renderer.h`, `host/vk/` | GPU renderer interface and its Vulkan backend (Windows): `device.c` (device, swapchain, frames, present), `textures.c` (images), `draw.c` (pipelines, draws) |
| `tools/recomp/host/build_windows.sh` | Windows .exe with llvm-mingw: cross (Linux) or native (Git Bash on Windows); resumable |
| `tools/recomp/sm86/` | SM86 shader -> GLSL translator; `batch.py` builds the renderer's SPIR-V cache |
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

Useful environment variables: `RECOMP_SPV_DIR` (shader cache, default `spv`), `RECOMP_GPU=0`
(no Vulkan: CPU clears + GDI), `RECOMP_VK_VALIDATION=1`, `RECOMP_DRAW_LOG=<n>` (first n draws in
full), `RECOMP_VK_DEBUG_FILL=1` (new images start in a colour picked from their address),
`RECOMP_NVN_LOG=<text>` (calls of generic NVN procs whose name contains text),
`RECOMP_NVN_LOG_BINDS=1`, `RECOMP_HLE_STATS=<n>` (HLE call statistics at frame 120),
`RECOMP_SAVE_DIR` (save data dir), `RECOMP_AUTO_PRESS=<n>`
(tap A every n polls, headless), `RECOMP_DUMP_FRAMES=<dir>` (PPM of presented frames),
`RECOMP_PROFILE=1` (interpreter profile every 300 frames), `RECOMP_NVN_TRACE=1`,
`RECOMP_IPC_TRACE=1`, `RECOMP_BLOCK_DISPATCH=1` (experimental, see open problems),
`RECOMP_WATCH=main+0x...` (dump words at Break). `-v` logs RomFS reads / IPC; `-vv` every SVC.

## Building on the owner's PC (Windows, native; set up 2026-10-27)

Tools live in `C:\Users\leozi\deps`: `llvm-mingw-20240619-ucrt-x86_64` (clang 18.1.8),
`unicorn-2.1.3` (source, for the headers), `unicorn-win` (`libunicorn.dll` from the cloud build
+ `libunicorn.dll.a` made with `gendef` / `llvm-dlltool`, + `libwinpthread-1.dll`),
`lasrecomp` (sparse clone of LAS Recomp's `runtime/nvn`, for reference). Python 3.12 has
`pip install capstone` (5.0.7). The LunarG Vulkan SDK 1.4.363 is installed (`$VULKAN_SDK`:
headers, `vulkan-1.lib`, `glslangValidator`). GPU: RTX 4070 (Vulkan 1.4 driver).
Work dir: `C:\Users\leozi\Desktop\NSW2 recomp` (`pound` = this repo, `work` = build outputs:
`bins/` NSO images, `gen/` C, `win/` exe + objects, `spv/` shader cache).
From Git Bash, in `work`:

```
D=/c/Users/leozi/Downloads/DonkeyKongBananza/DonkeyKongBananza/updates/458752
bin/nso_info.exe $D/exefs/main bins/main.bin                  # same for sdk, rtld
python ../pound/tools/recomp/recomp.py bins/main.bin --out gen/main --prefix main \
    --no-comments --per-file 400 --stats > gen/main.stats     # ~3 min; run the 3 in parallel
export MINGW=/c/Users/leozi/deps/llvm-mingw-20240619-ucrt-x86_64 UNICORN_SRC=/c/Users/leozi/deps/unicorn-2.1.3 \
       UNICORN_WIN=/c/Users/leozi/deps/unicorn-win            # VULKAN_SDK is set by the SDK installer
bash ../pound/tools/recomp/host/build_windows.sh gen win rtld sdk main   # -O1: ~3 min on 16 threads
cd win && RECOMP_SAVE_DIR=save RECOMP_SHADER_DUMP=shaders ./recomp_run.exe $D/exefs $D/romfs > run.log 2>&1
cd .. && python ../pound/tools/recomp/sm86/batch.py win/shaders spv "$VULKAN_SDK/Bin/glslangValidator.exe"
cd win && RECOMP_SAVE_DIR=save RECOMP_SPV_DIR=../spv ./recomp_run.exe $D/exefs $D/romfs > run.log 2>&1
```

Delete `win/obj/librecomp_<m>.a` (and `win/obj/<m>/`) to force a module rebuild; host
changes only relink (seconds). Runs open the game window; `timeout <s>` bounds test runs.

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

8. (2026-10-27, owner's PC) Native Windows build works (`recomp_run.exe` 168 MB at -O1) and
   runs the real dump. Vulkan renderer started (`host/vk/`, adapted from LAS Recomp): device
   and swapchain on the game window (RTX 4070), one VkImage per guest texture, clears and
   presents on the GPU (window crop honoured; `RECOMP_DUMP_FRAMES` reads frames back), and
   draws: command buffers record the full draw state (`hle_nvn.c`: program, vertex buffers /
   attribute / stream states, uniform / storage buffers, textures, pools, viewport, scissor,
   depth range, state objects, `UpdateUniformBuffer`), executed at present / `nvnQueueFinish`
   (deferred, like a GPU running behind). `vk/draw.c` builds pipelines from the sm86 SPIR-V
   cache and draws. Verified on the frame blit of the title stage: vertex data, matrix from
   `UpdateUniformBuffer`, source texture from the texture pool (`RECOMP_VK_DEBUG_FILL=1` shows
   the quad landing in the window texture); no validation errors.
   Findings: uniform buffer i = cbuf bank i + 3; texture i's handle at c[2][0x20 + 8 i]; window
   origin mode 1 (upper left: no flips needed), depth mode 0 (z from -w..w, converted in the
   vertex shader); NVN2 state objects keep the NVN1 sizes (vertex attrib 4 bytes, stream 8)
   and are configured through procs the generic fallback records as properties (the guest
   structs stay zero); window textures are 2880x1620 with a 1920x1080 crop.

## Plan (agreed 2026-10-27): move the work to the owner's PC

Steps 1 and 2 are done (see "Building on the owner's PC"); step 3 is under way (status 8 and
"Next steps for the renderer").

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

## Reference: how another Switch 2 port did it (Fire Emblem: Fortune's Weave)

Shared by its author ("Jacob") in a chat; his code is private, these are his notes. He forked
LAS Recomp (his BotW port) into a new project for FE on Switch 2.

- **Spotting Switch 2 shaders.** In `objects.c` (set_shaders), a blob without the Maxwell
  magic (0x12345678) is a Switch 2 shader. Tag 0x81 = vertex, 0x82 = fragment, anything else
  = compute; code starts at +0x80. Instructions are 128-bit (Ampere style), which
  `maxwell_translate` cannot decode. (Matches what we found in DKB.)
- **Ahead-of-time translation, not live.** Shaders from gameplay are dumped
  (`FE_SHADER_CAPTURE_DIR`, plus compute shaders that failed) and translated by Python tools:
  `sm86_fragment_glsl.py` (shared emitter), `sm86_vertex_glsl.py`, `sm86_compute.py`;
  `sm86_cache.py` builds the cache: GLSL plus a resource list per program, keyed by an
  FNV-1a hash of the whole program. On FE's prologue dialogue set: 56/56 vertex, 96/98
  fragment, 20/20 compute programs translate and compile.
- **Loading at runtime.** With `FE_SCENE_SM86_CACHE` set, the cache lookup (`s2_sm86_cache`,
  LAS `vulkan/exec.c`) is tried first in `translate_program` for draws and in the compute
  path. It must stand in for Switch 2 driver behaviour that LAS never modelled:
  - texture handles held in registers, served from a shared texture pool (descriptor set 2);
  - a rebuilt compute constant bank 0 (`s2_compute_bank0`);
  - the compute workgroup size, read from the program's control section at +0x58;
  - small storage buffers copied when commands are recorded.
  `device.c` enables the extra Vulkan features these programs need.
- **Fallbacks.** If a program is not in the cache, `translate_program` walks hand-written
  per-hash shaders (`s2_*_probe`, `s2_sm86_*`, headers `vulkan/sm86_*.h`), then
  `maxwell_translate` (which cannot handle SM86).
- **Other Switch 2 fixes outside shaders:**
  - the T239 block-linear tile byte order differs from Switch 1 (fixed in LAS `tegra.c`);
  - DLSS has a stub (`dlss.c`) that falls back to a plain upscale;
  - the memory budget had to be raised (6 GiB for FE; DKB needs 10 GB, see above).
- His recipe in short: dump a game, use IDA/Ghidra, start from LAS Recomp.

## Next steps for the renderer (in order)

1. Only one draw per frame is executed at the title stage: the main thread submits one
   list per frame (37 ops, the frame blit). Worker threads (0x12c..0x130) record a list per
   frame (15 ops, 1 draw) on command buffers 0x30....bd8 that are never submitted nor called
   (`RECOMP_LIST_TRACE=1` shows it). Those same command buffers get NVN2's
   `nvnCommandBufferSetRecordingMode(cmdbuf, mode 0/1, offset, 0xc00000, flags)` around 6
   times a frame, with offsets stepping by 0xc00000 (a triple-buffered 12 MB ring): the
   recording mode presumably makes them record into memory the main command buffer runs.
   Next: reverse that proc's semantics (Ghidra on the game's callers / the NVN2 driver in
   sdk), then execute those lists at the right place. (Also fixed: recorder slots are freed
   at `nvnCommandBufferFinalize`; not the cause.)
2. Guest texture uploads: textures the CPU fills (RomFS data) need a copy from guest memory
   with T239 block-linear deswizzling (LAS Recomp `tegra.c`; the byte order differs from
   Switch 1) and BCn/ASTC formats; samplers from `nvnSamplerPoolRegisterSampler`.
3. Stencil state, culling convention with the upper-left origin (`RECOMP_VK_CULL=1` to test),
   guest memory for LDG shaders (gmem binding 17; LAS imports the guest heap), compute
   dispatches, multi-layer / mip views, MSAA, queries / `ReportCounter`, events.
4. Performance: images keyed by storage address (aliasing), fewer barriers, pipeline cache
   on disk, a render thread.

## Open problems / next steps

1. Translator gaps: I2F sizes, TMML, TLD4, SHFL, F2FP/HADD2, PLOP3 with uniform preds, a
   texture used with two sampler types, compute shaders (32/36 fail).
2. Unknown yet: vertex output -> fragment varying mapping (assumed generic index i), what
   banks 0/1 hold (zeros are bound for now).
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
