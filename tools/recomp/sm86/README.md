# SM86 shader translator (Switch 2 / T239)

Switch 2 games hand NVN2 shader programs to the driver as GPU machine code for the Ampere
(SM86) architecture. The host renderer cannot run that code, so each program is translated
to Vulkan GLSL ahead of time.

- `isa.py`: opcode table and field helpers. The 128-bit encoding follows the public
  description of SM70+ instructions in Mesa's NAK compiler (`sm70_encode.rs`); opcodes NAK
  never emits were identified from DKB's own shaders (I2FP 0x045, FSEL 0x008, R2P/P2R,
  BRX 0x949, CALL 0x944, DEPBAR 0x91a, LDC with uniform offset 0xabb, IPA 0x526).
- `translate.py`: one blob -> GLSL + resource description. Blob layout (NVN2): 0x80-byte
  header (byte 0: 0x81 vertex, 0x82 fragment; word 0x48: fragment output mask), then code.
  Compute programs have no header.
- `batch.py <dump dir> <out dir> [glslangValidator]`: translate every captured program and
  compile it to SPIR-V; prints coverage. The out dir is the renderer's shader cache
  (`$RECOMP_SPV_DIR`): `<hash>.<vert|frag|comp>.spv` plus `<hash>.res`, a line-based
  resource description (`cbuf <bank> <bytes>`, `tex <bank> <offset> <glsl type> <binding>`,
  `in <location>`, `out <location>`, `gmem`).

Vertex shaders end with the NVN depth-mode conversion (clip z from -w..w to 0..w), switched
by specialization constant 10 (`DEPTH_MINUS_ONE`, on by default; DKB uses depth mode 0).

Resource mapping confirmed on DKB with the renderer (host/vk/draw.c): uniform buffer i of a
stage is constant bank i + 3 (as NVN1); bank 2 holds the stage's texture handles, texture i
at c[2][0x20 + 8 * i].

Capture: run the host with `RECOMP_SHADER_DUMP=<dir>`; every program the game creates is
written as `<fnv64 of first 1 KB>.code` (64 KB window) and `.ctrl` (control section).

Status on DKB (2604 programs created by the time the title menu runs): 2247 translate and
compile to SPIR-V (1356/1439 vertex, 887/1129 fragment, 4/36 compute). Correctness is not
verified yet: that needs the renderer.
