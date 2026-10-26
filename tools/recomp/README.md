# AArch64 → C static recompiler (experimental)

Goal: run Switch 2 game code (first target: Donkey Kong Bananza, update 458752)
by translating its ARM64 machine code to C ahead of time, instead of emulating
the CPU at run time. OS services and the GPU still need to be provided separately.

## Pipeline

1. `tools/nso_info <exefs/main> main.bin` decompresses an NSO into a flat image.
2. `analyze.py main.bin sdk.bin rtld.bin` scopes the work (see `docs/recomp/`).
3. `recomp.py main.bin --out gen/ --all --stats` writes C:
   - one `void f_<offset>(cpu_t *c)` per guest function (`.eh_frame_hdr` starts
     plus BL targets);
   - direct branches inside a function become `goto`, `bl` becomes a C call,
     `ret` becomes `return`;
   - LLVM jump tables (`adrp/add/adr/ldr{b,h}/add/br`) are recovered into `switch`;
   - other indirect branches go through `recomp_dispatch()` (address → function);
   - PAC instructions are no-ops, LSE atomics map to `__atomic_*` builtins;
   - anything not supported yet becomes a `recomp_trap()` call, so the output
     always compiles. `--stats` reports coverage.
4. `runtime/recomp_cpu.h`: register file (`cpu_t`), flags and memory helpers. Guest
   memory is a flat mapping at `recomp_mem_base`.

## Verification

`test/difftest.py` runs each recompiled function and the original machine code
under Unicorn (reference emulator, `UC_CPU_ARM64_MAX`) from identical random
states: registers, a heap filled with random data/pointers, and a stack.
Imports (GOT/PLT) are bound to a page of `ret` stubs on both sides. It then compares
all GPRs, SP, NZCV, vector registers, heap, image and caller stack.

```
recomp.py main.bin --out gen --funcs <list> --callee-depth 3 --per-file 100
cd gen && ls *.c | xargs -P$(nproc) -I{} clang -O1 -c -fPIC -fno-strict-aliasing -w \
    -include ../test/test_config.h -I../runtime {} -o {}.o
clang -O1 -c -fPIC -include ../test/test_config.h -I../runtime ../test/harness.c -o harness.o
clang -shared *.o -o libtest.so -lm
difftest.py main.bin gen/libtest.so <list> --trials 6
```

Dependencies: Capstone 5 (decoder) and Unicorn 2 (tests only), Python 3.

## Status (DKB update 458752, `main`)

- Integer, load/store, branches, jump tables, LSE atomics, CRC32 and scalar FP
  (with AArch64 NaN rules: operand priority, default NaN, FPCR.DN) are done.
- **98.2 % of the 143,010 functions translate with no unsupported instruction.**
  What remains is almost entirely NEON/SIMD vector code (`fmul`/`fadd` on vectors,
  `mov` lanes, `ext`, `dup`, `zip`, `tbl`…).
- Differential test, random sample of 604 functions (seed 2026), 6 trials each:
  397 of 398 testable functions pass (1,913 cases), 0 state mismatches. The single
  failure is an unsupported SIMD instruction in a callee. 206 functions could not
  be tested because the reference itself faults on random inputs.
- Not handled yet: FPCR rounding modes other than round-to-nearest, flush-to-zero,
  exclusive load/store (unused by this title), `svc` (needs the kernel layer),
  `mrs` of the system counter.
