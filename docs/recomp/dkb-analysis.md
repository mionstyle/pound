# Static recompilation scoping: Donkey Kong Bananza (update 458752)

Generated with `tools/recomp/analyze.py` on the decompressed exefs modules
(`tools/nso_info <nso> out.bin`). Title ID `040074A01BF12000`.

## Modules

| Module | .text | Instructions | Functions (eh_frame) | Notes |
|---|---|---|---|---|
| main | 27.5 MiB | 7.2 M | 142,180 (+830 outlined, no CFI) | game code |
| sdk  | 7.7 MiB | 2.0 M | 38,243 (+~8k without CFI) | Nintendo SDK, NVN driver, IPC clients |
| rtld | 12 KiB | 3 K | 34 | dynamic loader |

`main`'s header differs from Switch 1: word 0 is `1` instead of a branch, and code
starts at +0x20 with `pacibsp`. `rtld` keeps the classic `b` + MOD0 layout.

## Function discovery

`.eh_frame_hdr` covers 99.4 % of `main`'s functions. The rest are BL targets
(machine-outlined helpers) plus 3 code pointers. 265,559 packed RELR relocations
give 84,195 distinct code pointers (vtables, callbacks); all but 3 are known
eh_frame functions. **Function discovery is essentially solved.**

## Instruction set (main)

- **Pointer Authentication** (ARMv8.3): 74k `pacibsp`, 53k `retab`, 35k `autibsp`.
  For recompilation these can be no-ops (`retab` → `ret`).
- **LSE atomics** (ARMv8.1): `ldadd`, `ldset`, `cas`, `swp`… (~7k), and **RCpc** `ldapr*` (~20k).
  **No exclusive load/store (`ldxr`/`stxr`) at all**, so atomics map directly to C11 atomics.
- CRC32 (~7k), light NEON (`fmla`, `ld1`/`st1`, `tbl`), **no SVE**.
- System registers: only `TPIDR_EL0` (499) and `FPCR`.

## OS interface

- `main` imports 937 symbols: 929 come from `sdk`, and 8 are weak `glslc*` imports
  (runtime shader compiler, unused in retail).
- Largest import areas: `nn::audio` 227, `nn::os` 113, libc 78, `nn::codec` 71,
  `nn::hid` 57, `nn::movie` 52, `nn::fs` 27.
- `sdk` uses **57 distinct SVCs**, which is the whole kernel surface needed if `sdk` is
  recompiled too (recommended over HLE'ing 929 functions).

## GPU

- `main` only calls `nvnBootstrapLoader`. The NVN driver lives in `sdk`,
  references `GA102`/`GA10F` (Ampere), and talks to `/dev/nvhost-*` through a
  `nvdrv` `INvDrv2User` IPC interface (different name from Switch 1's).
- Two options: (a) HLE NVN at the API level by replacing `nvnBootstrapLoader` with
  a Vulkan-backed NVN implementation (skips GPU command emulation), or (b) LLE the
  driver and emulate Ampere command buffers. Both still need SM86 shader binaries
  translated to SPIR-V, since retail ships no GLSL.
