#!/usr/bin/env python3
"""Differential test: recompiled C vs. Unicorn (reference AArch64 emulator).

usage: difftest.py <module.bin> <libtest.so> <func list file> [--trials N] [--seed S]

For every function in the list and every trial, both sides start from the same
random register file and the same guest memory (module image, a heap full of
random data and pointers, a stack). After the function returns, all general
purpose registers, SP, NZCV and the heap / image / caller stack are compared.

Cases where the reference itself faults, loops, or calls something outside the
build are counted as "skipped": they say nothing about correctness.
"""
import argparse
import collections
import ctypes
import os
import random
import struct
import sys

sys.path.insert(0, os.environ.get("UNICORN_PY", "/home/claude/deps/unicorn/bindings/python"))
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_INVALID  # noqa: E402
from unicorn.arm64_const import *  # noqa: E402,F403

IMAGE_BASE = 0
HEAP, HEAP_SIZE = 0x2_0000_0000, 0x10_0000
STACK, STACK_SIZE = 0x3_0000_0000, 0x10_0000
SP0 = STACK + STACK_SIZE - 0x8_0000
RET_MAGIC = 0x4_0000_0000
STUB = 0x5_0000_0000  # page of `ret` instructions standing in for imports
RET_INSN = struct.pack("<I", 0xD65F03C0)
MAX_INSNS = 2_000_000


class VReg(ctypes.Union):
    _fields_ = [("b", ctypes.c_uint8 * 16), ("d", ctypes.c_uint64 * 2), ("f64", ctypes.c_double * 2)]


class Cpu(ctypes.Structure):
    _fields_ = [("x", ctypes.c_uint64 * 31), ("sp", ctypes.c_uint64), ("pc", ctypes.c_uint64),
                ("tpidr", ctypes.c_uint64), ("n", ctypes.c_uint8), ("z", ctypes.c_uint8),
                ("cf", ctypes.c_uint8), ("v", ctypes.c_uint8), ("fpcr", ctypes.c_uint32),
                ("fpsr", ctypes.c_uint32), ("q", VReg * 32)]


XREGS = [globals()[f"UC_ARM64_REG_X{i}"] for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
QREGS = [globals()[f"UC_ARM64_REG_Q{i}"] for i in range(32)]


def random_heap(rng):
    words = []
    for _ in range(HEAP_SIZE // 8):
        r = rng.random()
        if r < 0.55:
            words.append(HEAP + rng.randrange(0x1000, HEAP_SIZE - 0x1000) & ~7)
        elif r < 0.65:
            words.append(STUB + 4 * rng.randrange(0, 1024))
        elif r < 0.92:
            words.append(rng.randrange(0, 256))
        else:
            words.append(rng.getrandbits(64))
    return struct.pack(f"<{len(words)}Q", *words)


def random_state(rng):
    x = []
    for i in range(31):
        r = rng.random()
        if r < 0.6:
            x.append(HEAP + (rng.randrange(0x2000, HEAP_SIZE - 0x2000) & ~7))
        elif r < 0.95:
            x.append(rng.randrange(0, 64))
        else:
            x.append(rng.getrandbits(64))
    q = [rng.getrandbits(128) for _ in range(32)]
    nzcv = rng.randrange(16)
    tpidr = HEAP + 0x800
    return x, q, nzcv, tpidr


def run_reference(image, heap, stack, fn, x, q, nzcv, tpidr):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    mu.mem_map(IMAGE_BASE, (len(image) + 0xFFF) & ~0xFFF)
    mu.mem_write(IMAGE_BASE, image)
    mu.mem_map(HEAP, HEAP_SIZE)
    mu.mem_write(HEAP, heap)
    mu.mem_map(STACK, STACK_SIZE)
    mu.mem_write(STACK, stack)
    mu.mem_map(STUB, 0x1000)
    mu.mem_write(STUB, RET_INSN * 1024)
    for r, v in zip(XREGS, x):
        mu.reg_write(r, v)
    mu.reg_write(UC_ARM64_REG_X30, RET_MAGIC)
    mu.reg_write(UC_ARM64_REG_SP, SP0)
    for r, v in zip(QREGS, q):
        mu.reg_write(r, v)
    mu.reg_write(UC_ARM64_REG_NZCV, nzcv << 28)
    mu.reg_write(UC_ARM64_REG_TPIDR_EL0, tpidr)
    try:
        mu.emu_start(IMAGE_BASE + fn, RET_MAGIC, count=MAX_INSNS)
    except UcError as e:
        return None, f"reference fault: {e} at pc 0x{mu.reg_read(UC_ARM64_REG_PC):x}"
    if mu.reg_read(UC_ARM64_REG_PC) != RET_MAGIC:
        return None, "reference did not return (instruction limit)"
    out = {
        "x": [mu.reg_read(r) for r in XREGS],
        "sp": mu.reg_read(UC_ARM64_REG_SP),
        "nzcv": mu.reg_read(UC_ARM64_REG_NZCV) >> 28,
        "heap": bytes(mu.mem_read(HEAP, HEAP_SIZE)),
        "stack": bytes(mu.mem_read(SP0, STACK + STACK_SIZE - SP0)),
        "image": bytes(mu.mem_read(IMAGE_BASE, len(image))),
        "q": [mu.reg_read(r) for r in QREGS],
    }
    return out, None


class Recompiled:
    def __init__(self, lib_path, image):
        self.lib = ctypes.CDLL(lib_path)
        self.lib.h_map.restype = ctypes.c_void_p
        self.lib.h_map.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
        self.lib.h_run.argtypes = [ctypes.c_uint64, ctypes.POINTER(Cpu), ctypes.c_int]
        self.lib.h_last_msg.restype = ctypes.c_char_p
        assert self.lib.h_cpu_size() == ctypes.sizeof(Cpu), "cpu_t layout mismatch"
        assert self.lib.h_init() == 0
        self.image_len = len(image)
        self.image_ptr = self.lib.h_map(IMAGE_BASE, (len(image) + 0xFFF) & ~0xFFF)
        self.heap_ptr = self.lib.h_map(HEAP, HEAP_SIZE)
        self.stack_ptr = self.lib.h_map(STACK, STACK_SIZE)
        stub_ptr = self.lib.h_map(STUB, 0x1000)
        ctypes.memmove(stub_ptr, RET_INSN * 1024, 0x1000)
        ctypes.c_uint64.in_dll(self.lib, "h_stub_base").value = STUB
        self.image = image

    def run(self, heap, stack, fn, x, q, nzcv, tpidr):
        ctypes.memmove(self.image_ptr, self.image, len(self.image))
        ctypes.memmove(self.heap_ptr, heap, HEAP_SIZE)
        ctypes.memmove(self.stack_ptr, stack, STACK_SIZE)
        c = Cpu()
        for k, v in enumerate(x):
            c.x[k] = v
        c.x[30] = RET_MAGIC
        c.sp = SP0
        for k, v in enumerate(q):
            c.q[k].d[0] = v & (2**64 - 1)
            c.q[k].d[1] = v >> 64
        c.n, c.z, c.cf, c.v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
        c.tpidr = tpidr
        rc = self.lib.h_run(fn, ctypes.byref(c), 2000)
        if rc != 0:
            return None, rc, self.lib.h_last_msg().decode()
        out = {
            "x": list(c.x),
            "sp": c.sp,
            "nzcv": c.n << 3 | c.z << 2 | c.cf << 1 | c.v,
            "heap": ctypes.string_at(self.heap_ptr, HEAP_SIZE),
            "stack": ctypes.string_at(self.stack_ptr + (SP0 - STACK), STACK + STACK_SIZE - SP0),
            "image": ctypes.string_at(self.image_ptr, self.image_len),
            "q": [c.q[k].d[0] | c.q[k].d[1] << 64 for k in range(32)],
        }
        return out, 0, None


def bind_imports(image):
    """Points every symbol-based relocation (GOT / PLT / ABS64) at the stub page."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    import analyze
    tmp = "/tmp/_difftest_image.bin"
    open(tmp, "wb").write(image)
    mod = analyze.Module("main", tmp)
    img = bytearray(image)
    n = 0
    for off, rtype, sym, addend in mod.relocs:
        if rtype in (analyze.R_GLOB_DAT, analyze.R_JUMP_SLOT, analyze.R_ABS64) and off + 8 <= len(img):
            struct.pack_into("<Q", img, off, STUB + 4 * (sym % 1024))
            n += 1
    print(f"bound {n} import relocations to the stub page")
    return bytes(img)


def first_diff(a, b, base):
    for k in range(0, len(a), 8):
        if a[k:k + 8] != b[k:k + 8]:
            return f"0x{base + k:x}: ref {a[k:k + 8].hex()} recomp {b[k:k + 8].hex()}"
    return None


def compare(ref, rec):
    diffs = []
    for k in range(31):
        if k == 30:
            continue  # link register value after return is implementation detail of the harness
        if ref["x"][k] != rec["x"][k]:
            diffs.append(f"x{k}: ref 0x{ref['x'][k]:x} recomp 0x{rec['x'][k]:x}")
    if ref["sp"] != rec["sp"]:
        diffs.append(f"sp: ref 0x{ref['sp']:x} recomp 0x{rec['sp']:x}")
    if ref["nzcv"] != rec["nzcv"]:
        diffs.append(f"nzcv: ref {ref['nzcv']:04b} recomp {rec['nzcv']:04b}")
    for k in range(32):
        if ref["q"][k] != rec["q"][k]:
            diffs.append(f"q{k}: ref {ref['q'][k]:032x} recomp {rec['q'][k]:032x}")
            break
    for name, base in (("heap", HEAP), ("stack", SP0), ("image", IMAGE_BASE)):
        if ref[name] != rec[name]:
            diffs.append(f"{name} {first_diff(ref[name], rec[name], base)}")
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module")
    ap.add_argument("lib")
    ap.add_argument("funcs")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--show", type=int, default=15)
    args = ap.parse_args()

    image = bind_imports(open(args.module, "rb").read())
    funcs = [int(l.split()[0], 16) for l in open(args.funcs) if l.strip()]
    rec = Recompiled(args.lib, image)
    rng = random.Random(args.seed)
    stack = bytes(STACK_SIZE)

    stats = collections.Counter()
    per_func = {}
    failures = []
    for fn in funcs:
        result = "skip"
        for t in range(args.trials):
            heap = random_heap(rng)
            x, q, nzcv, tpidr = random_state(rng)
            ref, why = run_reference(image, heap, stack, fn, x, q, nzcv, tpidr)
            if ref is None:
                stats["skip: " + why.split(":")[0]] += 1
                continue
            got, rc, msg = rec.run(heap, stack, fn, x, q, nzcv, tpidr)
            if got is None:
                if rc == 3:  # called something we did not build
                    stats["skip: callee not in build"] += 1
                    continue
                stats["FAIL: recompiled side stopped"] += 1
                failures.append((fn, t, [f"rc={rc} {msg}"]))
                result = "fail"
                break
            d = compare(ref, got)
            if d:
                stats["FAIL: state mismatch"] += 1
                failures.append((fn, t, d))
                result = "fail"
                break
            stats["pass"] += 1
            result = "pass" if result != "fail" else result
        per_func[fn] = result

    fc = collections.Counter(per_func.values())
    print(f"functions: {len(funcs)}  pass {fc['pass']}  fail {fc['fail']}  untestable {fc['skip']}")
    print("cases: " + ", ".join(f"{k} {v}" for k, v in stats.most_common()))
    for fn, t, d in failures[:args.show]:
        print(f"  f_{fn:x} trial {t}: " + "; ".join(d[:4]))
    with open(args.funcs + ".result", "w") as f:
        for fn, r in per_func.items():
            f.write(f"{fn:x} {r}\n")


if __name__ == "__main__":
    main()
