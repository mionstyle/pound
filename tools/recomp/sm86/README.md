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
  compile it to SPIR-V; prints coverage.

Capture: run the host with `RECOMP_SHADER_DUMP=<dir>`; every program the game creates is
written as `<fnv64 of first 1 KB>.code` (64 KB window) and `.ctrl` (control section).

Status on DKB (2604 programs created by the time the title menu runs): 2247 translate and
compile to SPIR-V (1356/1439 vertex, 887/1129 fragment, 4/36 compute). Correctness is not
verified yet: that needs the renderer.
