#!/usr/bin/env python3
"""AArch64 -> C static recompiler (first iteration).

usage:
  recomp.py <module.bin> --out <dir> [--funcs 0x1234,0x5678 | --all] [--stats]

<module.bin> is a decompressed NSO image (tools/nso_info <nso> out.bin).
Function boundaries come from .eh_frame_hdr plus BL targets (see analyze.py).

Each guest function becomes `void f_<addr>(cpu_t *c)` (see runtime/recomp_cpu.h).
Instructions that are not supported yet are emitted as a trap, so the output always
compiles; --stats reports how many functions are fully translated.
"""
import argparse
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze  # noqa: E402

try:
    import capstone as cs
    from capstone import arm64 as A
except ImportError:  # local source build
    sys.path.insert(0, os.environ.get("CAPSTONE_PY", "/home/claude/deps/capstone/bindings/python"))
    import capstone as cs
    from capstone import arm64 as A

COND = {1: "EQ", 2: "NE", 3: "HS", 4: "LO", 5: "MI", 6: "PL", 7: "VS", 8: "VC",
        9: "HI", 10: "LS", 11: "GE", 12: "LT", 13: "GT", 14: "LE", 15: "AL", 16: "AL"}
SFT_LSL, SFT_MSL, SFT_LSR, SFT_ASR, SFT_ROR = 1, 2, 3, 4, 5
EXT = {1: ("uint8_t", False), 2: ("uint16_t", False), 3: ("uint32_t", False), 4: ("uint64_t", False),
       5: ("int8_t", True), 6: ("int16_t", True), 7: ("int32_t", True), 8: ("int64_t", True)}

NOPS = {"nop", "pacibsp", "paciasp", "autibsp", "autiasp", "pacibz", "paciaz", "autibz", "autiaz",
        "xpaclri", "bti", "yield", "dmb", "dsb", "isb", "prfm", "prfum", "clrex", "hint", "sevl",
        "pacia1716", "pacib1716", "autia1716", "autib1716", "csdb", "esb", "psb"}
RETS = {"ret", "retaa", "retab"}

ATOMIC_OPS = {"ldadd": "add", "ldclr": "clr", "ldset": "set", "ldeor": "eor", "swp": "swp",
              "stadd": "add", "stclr": "clr", "stset": "set", "steor": "eor"}


class Unsupported(Exception):
    pass


def rindex(name):
    if name in ("fp",):
        return 29
    if name in ("lr",):
        return 30
    return int(name[1:])


def is_gpr(name):
    return name[0] in "xw" and name not in ("xzr", "wzr") or name in ("sp", "wsp", "fp", "lr")


def bits_of(name):
    if name in ("sp", "xzr", "fp", "lr") or name[0] == "x":
        return 64
    if name in ("wsp", "wzr") or name[0] == "w":
        return 32
    raise Unsupported(f"register {name}")


def rget(name):
    if name in ("xzr", "wzr"):
        return "0"
    if name == "sp":
        return "c->sp"
    if name == "wsp":
        return "((uint32_t)c->sp)"
    n = rindex(name)
    if name[0] == "w":
        return f"((uint32_t)c->x[{n}])"
    return f"c->x[{n}]"


def rset(name, expr):
    if name in ("xzr", "wzr"):
        return f"(void)({expr});"
    if name == "sp":
        return f"c->sp = (uint64_t)({expr});"
    if name == "wsp":
        return f"c->sp = (uint32_t)({expr});"
    n = rindex(name)
    if name[0] == "w":
        return f"c->x[{n}] = (uint32_t)({expr});"
    return f"c->x[{n}] = (uint64_t)({expr});"


def fpreg(name):
    """b/h/s/d/q/v register -> (index, size in bytes)."""
    sizes = {"b": 1, "h": 2, "s": 4, "d": 8, "q": 16, "v": 16}
    if name[0] in sizes and name[1:].isdigit():
        return int(name[1:]), sizes[name[0]]
    raise Unsupported(f"fp register {name}")


class FunctionTranslator:
    def __init__(self, mod, start, end, func_set, md):
        self.mod, self.start, self.end, self.funcs, self.md = mod, start, end, func_set, md
        self.insns = list(md.disasm(mod.d[start:end], start))
        self.unsupported = collections.Counter()
        self.callees = set()
        self.has_indirect_jump = False

    def callees_preview(self):
        out = set()
        for i in self.insns:
            if i.id and i.mnemonic in ("bl", "b") and i.operands and i.operands[0].type == A.ARM64_OP_IMM:
                t = i.operands[0].imm
                if t in self.funcs and not (self.start <= t < self.end):
                    out.add(t)
        return out

    # -- operands ---------------------------------------------------------
    def reg(self, i, op):
        if op.type != A.ARM64_OP_REG:
            raise Unsupported("expected register operand")
        return i.reg_name(op.reg)

    def opval(self, i, op, bits):
        T = "uint64_t" if bits == 64 else "uint32_t"
        if op.type == A.ARM64_OP_IMM:
            imm = op.imm
            if op.shift.type == SFT_LSL:
                imm <<= op.shift.value
            elif op.shift.type:
                raise Unsupported("shifted immediate")
            return f"(({T})0x{imm & ((1 << bits) - 1):x}ull)"
        if op.type != A.ARM64_OP_REG:
            raise Unsupported("operand type")
        v = rget(i.reg_name(op.reg))
        if op.ext:
            ctype, signed = EXT[op.ext]
            e = f"(({T})(int64_t)({ctype})({v}))" if signed else f"(({T})({ctype})({v}))"
            if op.shift.type == SFT_LSL and op.shift.value:
                e = f"(({T})({e} << {op.shift.value}))"
            return e
        v = f"(({T}){v})"
        s, amt = op.shift.type, op.shift.value
        if not s or (s == SFT_LSL and amt == 0):
            return v
        if s == SFT_LSL:
            return f"(({T})({v} << {amt}))"
        if s == SFT_LSR:
            return f"({v} >> {amt})"
        if s == SFT_ASR:
            return f"asr{bits}({v}, {amt})"
        if s == SFT_ROR:
            return f"ror{bits}({v}, {amt})"
        raise Unsupported("shift type")

    def mem_addr(self, i, op):
        """Returns (address expression, writeback statement or None)."""
        m = op.mem
        base = i.reg_name(m.base)
        b = rget(base)
        if m.index:
            idx = i.reg_name(m.index)
            iv = rget(idx)
            if op.ext:
                ctype, signed = EXT[op.ext]
                iv = f"(uint64_t)(int64_t)({ctype})({iv})" if signed else f"(uint64_t)({ctype})({iv})"
            amt = op.shift.value if op.shift.type == SFT_LSL else 0
            return f"({b} + ((uint64_t)({iv}) << {amt}))", base
        return f"({b} + (uint64_t){m.disp}ll)", base

    def target_code(self, target, tail):
        """Control transfer to an absolute module offset outside straight-line flow."""
        if self.start <= target < self.end and tail:
            self.labels.add(target)
            return f"goto L_{target:x};"
        if target in self.funcs:
            self.callees.add(target)
            call = f"f_{target:x}(c);"
        else:
            call = f"recomp_dispatch(c, MB + 0x{target:x}ull);"
        return call + (" return;" if tail else "")

    # -- translation ------------------------------------------------------
    def translate(self):
        self.labels = set()
        self.jump_tables = 0
        body = []
        for k, i in enumerate(self.insns):
            self.cur_index = k
            try:
                lines = self.insn(i)
            except (Unsupported, IndexError, KeyError, ValueError, AttributeError) as e:
                key = i.mnemonic if isinstance(e, Unsupported) else f"{i.mnemonic} [{type(e).__name__}: {i.op_str}]"
                self.unsupported[key] += 1
                lines = [f'recomp_trap(c, MB + 0x{i.address:x}ull, "unsupported: {i.mnemonic} {i.op_str} ({e})");']
            body.append((i, lines))

        out = [f"void f_{self.start:x}(cpu_t *c)", "{"]
        for i, lines in body:
            if i.address in self.labels:
                out.append(f"L_{i.address:x}:;")
            out.append(f"    /* {i.address:08x}: {i.mnemonic} {i.op_str} */")
            out.extend("    " + l for l in lines)
        out.append(f'    recomp_trap(c, MB + 0x{self.end:x}ull, "fell off end of function");')
        out.append("}")
        return "\n".join(out)

    def find_jump_table(self, k):
        """Recognises the LLVM AArch64 jump table sequence ending at insns[k] (`br xT`):
            cmp   wI, #N ; b.hi default          (bound)
            adrp  xTab, page ; add xTab, xTab, #lo
            adr   xB, base
            ldr{b,h,sb,sh,,sw} wE, [xTab, xI, lsl #s]
            add   xT, xB, xE(, sxtw), lsl #2
            br    xT
        Returns the sorted list of target offsets, or None."""
        ins = self.insns
        br = ins[k]
        reg_t = br.reg_name(br.operands[0].reg)
        window = ins[max(0, k - 24):k]

        def last_def(name, before):
            for j in range(len(window) - 1 - before, -1, -1):
                w = window[j]
                if w.id and w.operands and w.operands[0].type == A.ARM64_OP_REG:
                    if w.reg_name(w.operands[0].reg) in (name, "w" + name[1:], "x" + name[1:]):
                        return j, w
            return None, None

        j, add = last_def(reg_t, 0)
        if add is None or add.mnemonic != "add" or len(add.operands) != 3 or add.operands[2].type != A.ARM64_OP_REG:
            return None
        rb = add.reg_name(add.operands[1].reg)
        re_ = add.reg_name(add.operands[2].reg)
        jb, adr = last_def(rb, len(window) - j)
        jl, ld = last_def(re_, len(window) - j)
        if adr is None or adr.mnemonic != "adr" or ld is None or not ld.mnemonic.startswith("ldr"):
            return None
        base = adr.operands[1].imm
        memop = ld.operands[1]
        if memop.type != A.ARM64_OP_MEM or not memop.mem.index:
            return None
        size, signed = {"ldrb": (1, False), "ldrsb": (1, True), "ldrh": (2, False), "ldrsh": (2, True),
                        "ldrsw": (4, True)}.get(ld.mnemonic, (None, None))
        if ld.mnemonic == "ldr":
            size, signed = (4, False) if ld.reg_name(ld.operands[0].reg)[0] == "w" else (None, None)
        if size is None:
            return None
        rtab = ld.reg_name(memop.mem.base)
        ridx = ld.reg_name(memop.mem.index)
        ja, addlo = last_def(rtab, len(window) - jl)
        if addlo is None:
            return None
        adrp = None
        if addlo.mnemonic == "add" and addlo.operands[2].type == A.ARM64_OP_IMM:
            jp, adrp = last_def(rtab, len(window) - ja)
            if adrp is None or adrp.mnemonic != "adrp":
                return None
            table = adrp.operands[1].imm + addlo.operands[2].imm
        elif addlo.mnemonic == "adrp":
            table = addlo.operands[1].imm
        else:
            return None
        # Bound: `cmp idx, #N` + conditional branch guarding the table sequence. Either the
        # branch skips to the default (b.hi / b.hs falling through into the sequence) or it
        # jumps to the sequence (b.ls / b.lo). The guard may be far from the br.
        seq_start = min(w.address for w in (adr, ld, addlo, adrp) if w is not None)
        count = None
        k_ins = {w.address: n for n, w in enumerate(ins)}
        idx_names = (ridx, "w" + ridx[1:], "x" + ridx[1:])
        for n in range(len(ins) - 1):
            w, nb = ins[n], ins[n + 1]
            if not (w.id and nb.id and w.mnemonic in ("cmp", "subs") and nb.mnemonic.startswith("b.")):
                continue
            if w.operands[-1].type != A.ARM64_OP_IMM or w.operands[-2].type != A.ARM64_OP_REG:
                continue
            if w.reg_name(w.operands[-2].reg) not in idx_names:
                continue
            N = w.operands[-1].imm
            tgt = nb.operands[0].imm
            cond = COND.get(nb.cc)
            falls_into = nb.address + 4 == seq_start or (nb.address + 4 < seq_start and all(
                ins[m].mnemonic not in ("b", "br", "ret", "retab") and not ins[m].mnemonic.startswith(("b.", "cb", "tb"))
                for m in range(n + 2, k_ins.get(seq_start, n + 2))))
            if tgt == seq_start and cond in ("LS", "LO"):
                count = N + 1 if cond == "LS" else N
            elif falls_into and cond in ("HI", "HS"):
                count = N + 1 if cond == "HI" else N
            if count is not None:
                break
        if count is None or count > 4096:
            return None
        d = self.mod.d
        fmt = {1: "b", 2: "h", 4: "i"}[size]
        if not signed:
            fmt = fmt.upper()
        targets = set()
        for n in range(count):
            off = table + n * size
            if off + size > len(d):
                return None
            e = struct.unpack_from("<" + fmt, d, off)[0]
            targets.add(base + 4 * e)
        if not all(self.start <= t < self.end for t in targets):
            return None
        return sorted(targets)

    def indirect_jump(self, tvar):
        k = self.cur_index
        table = self.find_jump_table(k)
        if table is not None:
            self.labels |= set(table)
            self.jump_tables += 1
            cases = " ".join(f"case 0x{t:x}: goto L_{t:x};" for t in table)
            return [f"{{ uint64_t t_ = {tvar} - MB; switch (t_) {{ {cases} }}",
                    f'  recomp_trap(c, {tvar}, "jump table target out of range"); }}']
        # Otherwise: an indirect tail call (e.g. virtual call after the epilogue).
        return [f"{{ uint64_t t_ = {tvar};",
                f"  if (t_ - MB >= 0x{self.start:x}ull && t_ - MB < 0x{self.end:x}ull) recomp_trap(c, t_, \"unresolved intra-function indirect jump\");",
                "  recomp_dispatch(c, t_); return; }"]

    def insn(self, i):
        mn = i.mnemonic
        if i.id == 0:  # skipdata
            raise Unsupported("data in code")
        ops = i.operands
        nxt = i.address + 4
        if mn in NOPS:
            return []
        if mn in RETS:
            return ["return;"]

        # ---- branches ---------------------------------------------------
        if mn == "b":
            return [self.target_code(ops[0].imm, tail=True)]
        if mn.startswith("b.") or (mn == "b" and i.cc):
            cond = COND[i.cc]
            return [f"if (C_{cond}) {{ {self.target_code(ops[0].imm, tail=True)} }}"]
        if mn in ("cbz", "cbnz"):
            r = rget(self.reg(i, ops[0]))
            test = "==" if mn == "cbz" else "!="
            return [f"if ({r} {test} 0) {{ {self.target_code(ops[1].imm, tail=True)} }}"]
        if mn in ("tbz", "tbnz"):
            r = rget(self.reg(i, ops[0]))
            bit = ops[1].imm
            test = "== 0" if mn == "tbz" else "!= 0"
            return [f"if ((({r}) >> {bit} & 1) {test}) {{ {self.target_code(ops[2].imm, tail=True)} }}"]
        if mn == "bl":
            return [f"c->x[30] = MB + 0x{nxt:x}ull;", self.target_code(ops[0].imm, tail=False)]
        if mn in ("blr", "blraa", "blrab", "blraaz", "blrabz"):
            r = rget(self.reg(i, ops[0]))
            return [f"{{ uint64_t t_ = {r}; c->x[30] = MB + 0x{nxt:x}ull; recomp_dispatch(c, t_); }}"]
        if mn in ("br", "braa", "brab", "braaz", "brabz"):
            return self.indirect_jump(rget(self.reg(i, ops[0])))

        # ---- system -----------------------------------------------------
        if mn == "svc":
            return [f"c->pc = MB + 0x{i.address:x}ull; recomp_svc(c, 0x{ops[0].imm:x});"]
        if mn in ("brk", "udf", "hlt"):
            return [f'recomp_trap(c, MB + 0x{i.address:x}ull, "{mn}");']
        if mn == "mrs":
            rd = self.reg(i, ops[0])
            sysreg = i.op_str.split(",")[1].strip().lower()
            src = {"tpidr_el0": "c->tpidr", "fpcr": "c->fpcr", "fpsr": "c->fpsr",
                   "nzcv": "((uint64_t)c->n << 31 | (uint64_t)c->z << 30 | (uint64_t)c->cf << 29 | (uint64_t)c->v << 28)"}.get(sysreg)
            if src is None:
                raise Unsupported(f"mrs {sysreg}")
            return [rset(rd, src)]
        if mn == "msr":
            parts = [p.strip().lower() for p in i.op_str.split(",")]
            if parts[0] in ("fpcr", "fpsr", "tpidr_el0") and len(ops) >= 1:
                src = rget(i.reg_name(ops[-1].reg))
                dst = {"fpcr": "c->fpcr", "fpsr": "c->fpsr", "tpidr_el0": "c->tpidr"}[parts[0]]
                return [f"{dst} = {src};"]
            raise Unsupported(f"msr {parts[0]}")

        # ---- loads / stores --------------------------------------------
        if mn.startswith(("ld", "st", "cas", "swp")) and mn not in ("stg",):
            return self.memory(i)

        # ---- moves / immediates ----------------------------------------
        if mn in ("mov", "movz", "movn"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd) or self.is_fp_op(i, ops[1]):
                return self.fp_mov(i)
            bits = bits_of(rd)
            if ops[1].type == A.ARM64_OP_IMM:
                imm = ops[1].imm
                if ops[1].shift.type == SFT_LSL:
                    imm <<= ops[1].shift.value
                if mn == "movn":
                    imm = ~imm
                return [rset(rd, f"0x{imm & ((1 << bits) - 1):x}ull")]
            return [rset(rd, rget(self.reg(i, ops[1])))]
        if mn == "movk":
            rd = self.reg(i, ops[0])
            sh = ops[1].shift.value if ops[1].shift.type == SFT_LSL else 0
            return [rset(rd, f"({rget(rd)} & ~(0xffffull << {sh})) | (0x{ops[1].imm:x}ull << {sh})")]
        if mn in ("adr", "adrp"):
            return [rset(self.reg(i, ops[0]), f"MB + 0x{ops[1].imm:x}ull")]

        # ---- arithmetic -------------------------------------------------
        if mn in ("add", "adds", "sub", "subs", "cmp", "cmn"):
            if mn in ("cmp", "cmn"):
                rd, rn_op, op2 = None, ops[0], ops[1]
            else:
                rd, rn_op, op2 = self.reg(i, ops[0]), ops[1], ops[2]
                if self.is_fp(rd):
                    raise Unsupported("vector add/sub")
            rn = self.reg(i, rn_op)
            bits = bits_of(rd or rn)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a = f"(({T}){rget(rn)})"
            b = self.opval(i, op2, bits)
            is_sub = mn in ("sub", "subs", "cmp")
            flags = mn in ("adds", "subs", "cmp", "cmn")
            if flags:
                expr = f"add{bits}f(c, {a}, ({T})~{b}, 1)" if is_sub else f"add{bits}f(c, {a}, {b}, 0)"
            else:
                expr = f"({a} - {b})" if is_sub else f"({a} + {b})"
            return [rset(rd, expr) if rd else f"(void){expr};"]
        if mn in ("neg", "negs"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            b = self.opval(i, ops[1], bits)
            if mn == "negs":
                return [rset(rd, f"add{bits}f(c, 0, ({T})~{b}, 1)")]
            return [rset(rd, f"({T})(0 - {b})")]
        if mn in ("adc", "adcs", "sbc", "sbcs", "ngc", "ngcs"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            if mn in ("ngc", "ngcs"):
                a, b = f"(({T})0)", f"(({T}){rget(self.reg(i, ops[1]))})"
                sub, flags = True, mn == "ngcs"
            else:
                a, b = f"(({T}){rget(self.reg(i, ops[1]))})", f"(({T}){rget(self.reg(i, ops[2]))})"
                sub, flags = mn.startswith("sbc"), mn.endswith("s")
            bb = f"({T})~{b}" if sub else b
            if flags:
                return [rset(rd, f"add{bits}f(c, {a}, {bb}, c->cf)")]
            return [rset(rd, f"({T})({a} + {bb} + c->cf)")]

        # ---- logic ------------------------------------------------------
        if mn in ("and", "ands", "orr", "eor", "bic", "bics", "orn", "eon", "tst", "mvn"):
            if mn == "tst":
                rd, a_op, b_op = None, ops[0], ops[1]
            elif mn == "mvn":
                rd, a_op, b_op = self.reg(i, ops[0]), None, ops[1]
            else:
                rd, a_op, b_op = self.reg(i, ops[0]), ops[1], ops[2]
            if rd and self.is_fp(rd):
                raise Unsupported("vector logic")
            bits = bits_of(rd or self.reg(i, a_op))
            T = "uint64_t" if bits == 64 else "uint32_t"
            b = self.opval(i, b_op, bits)
            a = f"(({T}){rget(self.reg(i, a_op))})" if a_op is not None else None
            expr = {"and": f"({a} & {b})", "ands": f"({a} & {b})", "tst": f"({a} & {b})",
                    "orr": f"({a} | {b})", "eor": f"({a} ^ {b})", "bic": f"({a} & ({T})~{b})",
                    "bics": f"({a} & ({T})~{b})", "orn": f"({a} | ({T})~{b})", "eon": f"({a} ^ ({T})~{b})",
                    "mvn": f"(({T})~{b})"}[mn]
            if mn in ("ands", "bics", "tst"):
                expr = f"logic{bits}f(c, {expr})"
            return [rset(rd, expr) if rd else f"(void){expr};"]

        # ---- shifts -----------------------------------------------------
        if mn in ("lsl", "lsr", "asr", "ror", "lslv", "lsrv", "asrv", "rorv"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd):
                raise Unsupported("vector shift")
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a = f"(({T}){rget(self.reg(i, ops[1]))})"
            if ops[2].type == A.ARM64_OP_IMM:
                s = str(ops[2].imm)
            else:
                s = f"(unsigned)({rget(self.reg(i, ops[2]))} & {bits - 1})"
            kind = mn[:3]
            expr = {"lsl": f"({T})({a} << {s})", "lsr": f"({a} >> {s})",
                    "asr": f"asr{bits}({a}, {s})", "ror": f"ror{bits}({a}, {s})"}[kind]
            return [rset(rd, expr)]

        # ---- multiply / divide -----------------------------------------
        if mn in ("mul", "madd", "msub", "mneg"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd):
                raise Unsupported("vector mul")
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a, b = (f"(({T}){rget(self.reg(i, o))})" for o in ops[1:3])
            if mn == "mul":
                return [rset(rd, f"({T})({a} * {b})")]
            if mn == "mneg":
                return [rset(rd, f"({T})(0 - {a} * {b})")]
            acc = f"(({T}){rget(self.reg(i, ops[3]))})"
            return [rset(rd, f"({T})({acc} {'+' if mn == 'madd' else '-'} {a} * {b})")]
        if mn in ("smull", "umull", "smaddl", "umaddl", "smsubl", "umsubl", "smnegl", "umnegl"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd) or bits_of(rd) != 64:
                raise Unsupported("vector long multiply")
            ext = "(uint64_t)(int64_t)(int32_t)" if mn[0] == "s" else "(uint64_t)(uint32_t)"
            a, b = (f"{ext}({rget(self.reg(i, o))})" for o in ops[1:3])
            prod = f"({a} * {b})"
            if mn in ("smull", "umull"):
                return [rset(rd, prod)]
            if mn.endswith("negl"):
                return [rset(rd, f"(0 - {prod})")]
            acc = rget(self.reg(i, ops[3]))
            return [rset(rd, f"({acc} {'+' if 'add' in mn else '-'} {prod})")]
        if mn in ("smulh", "umulh"):
            rd = self.reg(i, ops[0])
            return [rset(rd, f"{mn[0]}mulh64({rget(self.reg(i, ops[1]))}, {rget(self.reg(i, ops[2]))})")]
        if mn in ("sdiv", "udiv"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            S = f"int{bits}_t" if mn == "sdiv" else f"uint{bits}_t"
            return [rset(rd, f"({'u' if mn == 'udiv' else ''}int{bits}_t){mn}{bits}(({S}){rget(self.reg(i, ops[1]))}, ({S}){rget(self.reg(i, ops[2]))})")]

        # ---- bitfield ---------------------------------------------------
        if mn in ("ubfx", "sbfx", "ubfiz", "sbfiz", "bfi", "bfxil", "bfc", "ubfm", "sbfm", "bfm"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            if mn == "bfc":
                src, lsb, width = "0", ops[1].imm, ops[2].imm
            else:
                src = rget(self.reg(i, ops[1]))
                lsb, width = ops[2].imm, ops[3].imm
            if mn in ("ubfm", "sbfm", "bfm"):
                immr, imms = lsb, width
            elif mn in ("ubfx", "sbfx", "bfxil"):
                immr, imms = lsb, lsb + width - 1
            else:  # *bfiz, bfi, bfc
                immr, imms = (-lsb) % bits, width - 1
            if mn.startswith("bf"):
                return [rset(rd, f"bfm({rget(rd)}, {src}, {immr}, {imms}, {bits})")]
            fn = "sbfm" if mn.startswith("s") else "ubfm"
            return [rset(rd, f"{fn}({src}, {immr}, {imms}, {bits})")]
        if mn in ("sxtb", "sxth", "sxtw", "uxtb", "uxth", "uxtw"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            ctype = {"sxtb": "int8_t", "sxth": "int16_t", "sxtw": "int32_t",
                     "uxtb": "uint8_t", "uxth": "uint16_t", "uxtw": "uint32_t"}[mn]
            mid = "(int64_t)" if mn[0] == "s" else ""
            return [rset(rd, f"(uint{bits}_t){mid}({ctype})({rget(self.reg(i, ops[1]))})")]
        if mn == "extr":
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            hi, lo = (f"(({T}){rget(self.reg(i, o))})" for o in ops[1:3])
            lsb = ops[3].imm
            if lsb == 0:
                return [rset(rd, lo)]
            return [rset(rd, f"({T})(({lo} >> {lsb}) | ({hi} << {bits - lsb}))")]
        if mn in ("clz", "cls", "rbit", "rev", "rev16", "rev32", "rev64"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd):
                raise Unsupported("vector bit op")
            bits = bits_of(rd)
            src = f"((uint{bits}_t){rget(self.reg(i, ops[1]))})"
            if mn in ("clz", "cls", "rbit"):
                return [rset(rd, f"{mn}{bits}({src})")]
            if mn in ("rev", "rev64") and bits == 64:
                return [rset(rd, f"__builtin_bswap64({src})")]
            if mn == "rev" and bits == 32:
                return [rset(rd, f"__builtin_bswap32({src})")]
            if mn == "rev16":
                return [rset(rd, f"(uint{bits}_t)rev16_64({src})")]
            if mn == "rev32" and bits == 64:
                return [rset(rd, f"rev32_64({src})")]
            raise Unsupported("rev variant")

        # ---- conditional ------------------------------------------------
        if mn in ("csel", "csinc", "csinv", "csneg"):
            rd = self.reg(i, ops[0])
            if self.is_fp(rd):
                raise Unsupported("fcsel")
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a, b = (f"(({T}){rget(self.reg(i, o))})" for o in ops[1:3])
            alt = {"csel": b, "csinc": f"({T})({b} + 1)", "csinv": f"({T})~{b}", "csneg": f"({T})(0 - {b})"}[mn]
            return [rset(rd, f"C_{COND[i.cc]} ? {a} : {alt}")]
        if mn in ("cset", "csetm"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            val = "1" if mn == "cset" else f"(uint{bits}_t)~0ull"
            return [rset(rd, f"C_{COND[i.cc]} ? {val} : 0")]
        if mn in ("cinc", "cinv", "cneg"):
            rd = self.reg(i, ops[0])
            bits = bits_of(rd)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a = f"(({T}){rget(self.reg(i, ops[1]))})"
            alt = {"cinc": f"({T})({a} + 1)", "cinv": f"({T})~{a}", "cneg": f"({T})(0 - {a})"}[mn]
            return [rset(rd, f"C_{COND[i.cc]} ? {alt} : {a}")]
        if mn in ("ccmp", "ccmn"):
            rn = self.reg(i, ops[0])
            if self.is_fp(rn):
                raise Unsupported("fccmp")
            bits = bits_of(rn)
            T = "uint64_t" if bits == 64 else "uint32_t"
            a = f"(({T}){rget(rn)})"
            b = self.opval(i, ops[1], bits)
            nzcv = ops[2].imm
            cmp = f"add{bits}f(c, {a}, ({T})~{b}, 1)" if mn == "ccmp" else f"add{bits}f(c, {a}, {b}, 0)"
            return [f"if (C_{COND[i.cc]}) (void){cmp}; else setnzcv(c, {nzcv});"]

        # ---- CRC32 ------------------------------------------------------
        if mn.startswith("crc32"):
            rd = self.reg(i, ops[0])
            size = {"b": 1, "h": 2, "w": 4, "x": 8}[mn[-1]]
            poly = "0x82F63B78u" if mn.startswith("crc32c") else "0xEDB88320u"
            return [rset(rd, f"crc32_upd((uint32_t){rget(self.reg(i, ops[1]))}, {rget(self.reg(i, ops[2]))}, {size}, {poly})")]

        if mn.startswith("f") or mn in ("scvtf", "ucvtf", "movi", "mvni") or self.any_fp(i):
            return self.fp(i)

        raise Unsupported("not implemented")

    # -- memory -------------------------------------------------------------
    def is_fp(self, name):
        return name[0] in "bhsdqv" and name[1:].isdigit()

    def is_fp_op(self, i, op):
        return op.type == A.ARM64_OP_REG and self.is_fp(i.reg_name(op.reg))

    def any_fp(self, i):
        return any(self.is_fp_op(i, o) for o in i.operands)

    def load_expr(self, size, signed, dest_bits, addr):
        raw = {1: f"rd8({addr})", 2: f"rd16({addr})", 4: f"rd32({addr})", 8: f"rd64({addr})"}[size]
        if signed:
            st = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[size]
            return f"(uint{dest_bits}_t)(int64_t)({st}){raw}"
        return raw

    def memory(self, i):
        mn = i.mnemonic
        ops = i.operands

        # Atomics (LSE).
        for base, kind in ATOMIC_OPS.items():
            if mn.startswith(base):
                return self.atomic(i, base, kind)
        if mn.startswith("cas"):
            return self.cas(i)

        if mn.startswith(("ldx", "ldax", "stx", "stlx", "ldnf", "ld1", "ld2", "ld3", "ld4",
                          "st1", "st2", "st3", "st4")):
            raise Unsupported("exclusive / vector structure access")

        pair = mn in ("ldp", "stp", "ldnp", "stnp", "ldpsw")
        is_load = mn.startswith("ld")
        nregs = 2 if pair else 1
        regs = [i.reg_name(o.reg) for o in ops[:nregs]]
        rest = ops[nregs:]

        # literal load: ldr xN, #imm
        if rest and rest[0].type == A.ARM64_OP_IMM and is_load and not pair:
            addr = f"(MB + 0x{rest[0].imm:x}ull)"
            wb_reg, post = None, None
        else:
            memop = rest[0]
            if memop.type != A.ARM64_OP_MEM:
                raise Unsupported("memory operand")
            post = rest[1].imm if len(rest) > 1 and rest[1].type == A.ARM64_OP_IMM else None
            addr, wb_reg = self.mem_addr(i, memop)
            if post is not None:
                wb_expr = f"{rget(wb_reg)} + (uint64_t){post}ll"
                addr = f"({rget(wb_reg)})"
            elif i.writeback:
                wb_expr = "a_"
            if not i.writeback:
                wb_reg = None

        # Access size per register.
        stem = mn.replace("ldur", "ldr").replace("stur", "str").replace("ldtr", "ldr").replace("sttr", "str")
        stem = stem.replace("ldapur", "ldr").replace("stlur", "str").replace("ldapr", "ldr").replace("ldar", "ldr").replace("stlr", "str")
        lines = [f"{{ uint64_t a_ = {addr};"]
        signed = False
        for k, r in enumerate(regs):
            fp = self.is_fp(r)
            if fp:
                idx, size = fpreg(r)
            else:
                rb = bits_of(r)
                if stem in ("ldrb", "strb", "ldrsb"):
                    size = 1
                elif stem in ("ldrh", "strh", "ldrsh"):
                    size = 2
                elif stem in ("ldrsw", "ldpsw"):
                    size = 4
                else:
                    size = rb // 8
                signed = stem in ("ldrsb", "ldrsh", "ldrsw", "ldpsw")
            off = f" + {k * size}" if k else ""
            a = f"a_{off}"
            if is_load:
                if fp:
                    lines.append(f"  rdv({a}, &c->q[{idx}], {size});")
                else:
                    lines.append(f"  {{ uint64_t v_ = {self.load_expr(size, signed, bits_of(r), a)}; @W{k}@ }}")
                    lines[-1] = lines[-1].replace(f"@W{k}@", rset(r, "v_"))
            else:
                if fp:
                    lines.append(f"  wrv({a}, &c->q[{idx}], {size});")
                else:
                    lines.append(f"  wr{size * 8}({a}, (uint{size * 8}_t){rget(r)});")
        if wb_reg:
            lines.append(f"  {rset(wb_reg, wb_expr)}")
        lines.append("}")
        # Loads into the base register with writeback are UNPREDICTABLE; ignore.
        return lines

    def atomic(self, i, base, kind):
        mn = i.mnemonic
        rest = mn[len(base):]
        size_ch = rest[-1] if rest and rest[-1] in "bh" else None
        ops = i.operands
        if base.startswith("st"):
            rs, rt, memop = i.reg_name(ops[0].reg), None, ops[1]
        else:
            rs, rt, memop = i.reg_name(ops[0].reg), i.reg_name(ops[1].reg), ops[2]
        size = {"b": 1, "h": 2}.get(size_ch, bits_of(rs) // 8)
        T = f"uint{size * 8}_t"
        addr, _ = self.mem_addr(i, memop)
        p = f"({T} *)R_HOST(a_, {size})"
        v = f"({T}){rget(rs)}"
        op = {"add": f"__atomic_fetch_add({p}, {v}, __ATOMIC_SEQ_CST)",
              "clr": f"__atomic_fetch_and({p}, ({T})~{v}, __ATOMIC_SEQ_CST)",
              "set": f"__atomic_fetch_or({p}, {v}, __ATOMIC_SEQ_CST)",
              "eor": f"__atomic_fetch_xor({p}, {v}, __ATOMIC_SEQ_CST)",
              "swp": f"__atomic_exchange_n({p}, {v}, __ATOMIC_SEQ_CST)"}[kind]
        lines = [f"{{ uint64_t a_ = {addr}; {T} old_ = {op};"]
        if rt:
            lines.append(f"  {rset(rt, 'old_')}")
        lines.append("}")
        return lines

    def cas(self, i):
        mn = i.mnemonic
        if mn.startswith("casp"):
            raise Unsupported("casp")
        rest = mn[3:]
        size_ch = rest[-1] if rest and rest[-1] in "bh" else None
        ops = i.operands
        rs, rt = i.reg_name(ops[0].reg), i.reg_name(ops[1].reg)
        size = {"b": 1, "h": 2}.get(size_ch, bits_of(rs) // 8)
        T = f"uint{size * 8}_t"
        addr, _ = self.mem_addr(i, ops[2])
        return [f"{{ uint64_t a_ = {addr}; {T} exp_ = ({T}){rget(rs)};",
                f"  __atomic_compare_exchange_n(({T} *)R_HOST(a_, {size}), &exp_, ({T}){rget(rt)}, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);",
                f"  {rset(rs, 'exp_')} }}"]

    # -- floating point (minimal) -------------------------------------------
    def fp_mov(self, i):
        ops = i.operands
        d = i.reg_name(ops[0].reg)
        if ops[1].type == A.ARM64_OP_REG:
            s = i.reg_name(ops[1].reg)
            if self.is_fp(d) and self.is_fp(s):
                di, ds = fpreg(d)
                si, ss = fpreg(s)
                if ds != ss or i.op_str.count(".") or "[" in i.op_str:
                    raise Unsupported("vector mov")
                return [f"{{ vreg_t t_ = {{0}}; memcpy(&t_, &c->q[{si}], {ss}); c->q[{di}] = t_; }}"]
            if self.is_fp(d):  # gpr -> fp
                di, ds = fpreg(d)
                if "[" in i.op_str:
                    raise Unsupported("vector element insert")
                return [f"{{ uint64_t v_ = {rget(s)}; memset(&c->q[{di}], 0, 16); memcpy(&c->q[{di}], &v_, {ds}); }}"]
            si, ss = fpreg(s)  # fp -> gpr
            if "[" in i.op_str and not i.op_str.endswith("[1]"):
                pass
            if "[" in i.op_str:
                raise Unsupported("vector element extract")
            return [f"{{ uint64_t v_ = 0; memcpy(&v_, &c->q[{si}], {ss}); {rset(d, 'v_')} }}"]
        raise Unsupported("fmov immediate")

    def fp(self, i):
        mn = i.mnemonic
        ops = i.operands
        if mn in ("movi", "mvni") or (mn == "fmov" and re.search(r"\bv\d+\.", i.op_str)):
            return self.vector_const(i)
        if re.search(r"\bv\d+\.", i.op_str) or "[" in i.op_str:
            raise Unsupported("SIMD vector")
        if mn == "fmov":
            if ops[1].type in (A.ARM64_OP_FP, A.ARM64_OP_IMM):
                d = i.reg_name(ops[0].reg)
                di, ds = fpreg(d)
                val = ops[1].fp if ops[1].type == A.ARM64_OP_FP else 0.0
                ctype = {4: "float", 8: "double"}.get(ds)
                if not ctype:
                    raise Unsupported("fmov half")
                return [f"{{ {ctype} f_ = ({ctype}){val!r}; memset(&c->q[{di}], 0, 16); memcpy(&c->q[{di}], &f_, {ds}); }}"]
            return self.fp_mov(i)

        def F(name):
            idx, size = fpreg(name)
            if size == 4:
                return f"c->q[{idx}].f32[0]", "f32", idx, 4
            if size == 8:
                return f"c->q[{idx}].f64[0]", "f64", idx, 8
            raise Unsupported("half precision")

        def setf(name, expr):
            ref, S, idx, size = F(name)
            ctype = "float" if size == 4 else "double"
            return f"{{ {ctype} r_ = {expr}; memset(&c->q[{idx}], 0, 16); c->q[{idx}].{S}[0] = r_; }}"

        regs = [i.reg_name(o.reg) for o in ops if o.type == A.ARM64_OP_REG]

        binops = {"fadd": "add", "fsub": "sub", "fmul": "mul", "fdiv": "div", "fmax": "max",
                  "fmin": "min", "fmaxnm": "maxnm", "fminnm": "minnm"}
        if mn in binops or mn in ("fnmul", "fabd"):
            d, a, b = regs[:3]
            fa, S, _, _ = F(a)
            fb = F(b)[0]
            if mn == "fnmul":
                return [setf(d, f"{S}_neg({S}_mul(c, {fa}, {fb}))")]
            if mn == "fabd":
                return [setf(d, f"{S}_abs({S}_sub(c, {fa}, {fb}))")]
            return [setf(d, f"{S}_{binops[mn]}(c, {fa}, {fb})")]
        if mn in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
            d, n, m, a = regs[:4]
            fn_, S, _, _ = F(n)
            fm, fa = F(m)[0], F(a)[0]
            neg_a = mn in ("fnmadd", "fnmsub")
            neg_n = mn in ("fmsub", "fnmadd")
            A_ = f"{S}_neg({fa})" if neg_a else fa
            N_ = f"{S}_neg({fn_})" if neg_n else fn_
            return [setf(d, f"{S}_muladd(c, {A_}, {N_}, {fm})")]
        if mn in ("fneg", "fabs", "fsqrt"):
            d, a = regs[:2]
            fa, S, _, _ = F(a)
            expr = {"fneg": f"{S}_neg({fa})", "fabs": f"{S}_abs({fa})", "fsqrt": f"{S}_sqrt(c, {fa})"}[mn]
            return [setf(d, expr)]
        if mn.startswith("frint"):
            d, a = regs[:2]
            fa, S, _, sz = F(a)
            sfx = "f" if sz == 4 else ""
            fn = {"frintm": f"floor{sfx}", "frintp": f"ceil{sfx}", "frintz": f"trunc{sfx}",
                  "frinta": f"round{sfx}", "frintn": f"__builtin_roundeven{sfx}", "frintx": f"rint{sfx}",
                  "frinti": f"rint{sfx}"}.get(mn)
            if fn is None:
                raise Unsupported("frint variant")
            return [setf(d, f"{S}_isnan({fa}) ? {S}_quiet(c, {fa}) : {fn}({fa})")]
        if mn == "fcvt":
            d, a = regs[:2]
            fa, Sa, _, _ = F(a)
            ref, Sd, _, sz = F(d)
            ctype = "float" if sz == 4 else "double"
            return [setf(d, f"{Sa}_isnan({fa}) ? (c->fpcr & FPCR_DN ? {Sd.upper()}_DNAN : ({ctype}){Sa}_quiet(c, {fa})) : ({ctype}){fa}")]
        if mn in ("fcmp", "fcmpe"):
            fa, S, _, _ = F(regs[0])
            fb = F(regs[1])[0] if len(regs) > 1 else "0.0"
            return [f"{S}_cmp(c, {fa}, {fb});"]
        if mn in ("fccmp", "fccmpe"):
            fa, S, _, _ = F(regs[0])
            fb = F(regs[1])[0]
            nzcv = [o.imm for o in ops if o.type == A.ARM64_OP_IMM][0]
            return [f"if (C_{COND[i.cc]}) {S}_cmp(c, {fa}, {fb}); else setnzcv(c, {nzcv});"]
        if mn == "fcsel":
            d, a, b = regs[:3]
            return [setf(d, f"C_{COND[i.cc]} ? {F(a)[0]} : {F(b)[0]}")]
        fbits = [o.imm for o in ops if o.type == A.ARM64_OP_IMM]
        fbits = fbits[0] if fbits else 0
        if mn in ("scvtf", "ucvtf"):
            d, s = regs[:2]
            if self.is_fp(s):  # integer in the low lane of an FP register (same width)
                si, ss = fpreg(s)
                bits = ss * 8
                src = f"c->q[{si}].{'s' if bits == 32 else 'd'}[0]"
            else:
                bits = bits_of(s)
                src = rget(s)
            st = f"{'u' if mn[0] == 'u' else ''}int{bits}_t"
            ctype = "float" if F(d)[3] == 4 else "double"
            expr = f"({ctype})({st}){src}"
            if fbits:  # exact scaling by a power of two, rounding happens once in the cast
                expr = f"({ctype})((long double)({st}){src} / (long double){1 << fbits}.0L)"
            return [setf(d, expr)]
        if mn in ("fcvtzs", "fcvtzu"):
            d, s = regs[:2]
            fs, S, _, ssz = F(s)
            if fbits:
                fs = f"(({'float' if ssz == 4 else 'double'})({fs} * {float(1 << fbits)!r}))"
            if self.is_fp(d):
                di, dsz = fpreg(d)
                bits = dsz * 8
                fn = f"recomp_{mn}{bits}_{'f32' if ssz == 4 else 'f64'}"
                return [f"{{ uint{bits}_t v_ = (uint{bits}_t){fn}({fs}); memset(&c->q[{di}], 0, 16); memcpy(&c->q[{di}], &v_, {dsz}); }}"]
            bits = bits_of(d)
            fn = f"recomp_{mn}{bits}_{'f32' if ssz == 4 else 'f64'}"
            return [rset(d, f"{fn}({fs})")]
        raise Unsupported("fp not implemented")

    def vector_const(self, i):
        """movi / mvni / fmov (vector immediate)."""
        mn = i.mnemonic
        ops = i.operands
        d = i.reg_name(ops[0].reg)
        di, _ = fpreg(d)
        m = re.search(r"\bv\d+\.(\d+)([bhsd])", i.op_str)
        if m:
            count, esize = int(m.group(1)), {"b": 8, "h": 16, "s": 32, "d": 64}[m.group(2)]
        else:  # scalar form, e.g. movi d2, #0
            count, esize = 1, 64
        imm_op = ops[1]
        if mn == "fmov":
            import struct as _s
            if esize == 32:
                val = _s.unpack("<I", _s.pack("<f", imm_op.fp))[0]
            elif esize == 64:
                val = _s.unpack("<Q", _s.pack("<d", imm_op.fp))[0]
            else:
                raise Unsupported("fmov half vector")
        else:
            val = imm_op.imm & ((1 << 64) - 1)
            sh = imm_op.shift
            if sh.type == SFT_LSL:
                val <<= sh.value
            elif sh.type == SFT_MSL:
                val = (val << sh.value) | ((1 << sh.value) - 1)
            if mn == "mvni":
                val = ~val
            val &= (1 << esize) - 1
        total = 0
        for k in range(count):
            total |= val << (k * esize)
        lo, hi = total & ((1 << 64) - 1), (total >> 64) & ((1 << 64) - 1)
        return [f"c->q[{di}].d[0] = 0x{lo:x}ull; c->q[{di}].d[1] = 0x{hi:x}ull;"]


def build_function_list(mod, text_end):
    eh = mod.eh_frame_functions()
    bl, _, _ = mod.scan_code()
    starts = sorted(s for s in (eh | bl) if 0 <= s < text_end)
    bounds = {}
    for k, s in enumerate(starts):
        bounds[s] = starts[k + 1] if k + 1 < len(starts) else text_end
    return bounds


PRELUDE = """// Generated by tools/recomp/recomp.py. Do not edit.
#include "recomp_cpu.h"
#include <math.h>

extern uint64_t recomp_module_base;
#define MB recomp_module_base

static inline int64_t  recomp_fcvtzs64_f64(double v) { if (v != v) return 0; if (v >= 9223372036854775807.0) return INT64_MAX; if (v <= -9223372036854775808.0) return INT64_MIN; return (int64_t)v; }
static inline int32_t  recomp_fcvtzs32_f64(double v) { if (v != v) return 0; if (v >= 2147483647.0) return INT32_MAX; if (v <= -2147483648.0) return INT32_MIN; return (int32_t)v; }
static inline int64_t  recomp_fcvtzs64_f32(float v)  { return recomp_fcvtzs64_f64(v); }
static inline int32_t  recomp_fcvtzs32_f32(float v)  { return recomp_fcvtzs32_f64(v); }
static inline uint64_t recomp_fcvtzu64_f64(double v) { if (v != v || v <= 0) return 0; if (v >= 18446744073709551615.0) return UINT64_MAX; return (uint64_t)v; }
static inline uint32_t recomp_fcvtzu32_f64(double v) { if (v != v || v <= 0) return 0; if (v >= 4294967295.0) return UINT32_MAX; return (uint32_t)v; }
static inline uint64_t recomp_fcvtzu64_f32(float v)  { return recomp_fcvtzu64_f64(v); }
static inline uint32_t recomp_fcvtzu32_f32(float v)  { return recomp_fcvtzu32_f64(v); }
"""


def finalize(code, tr):
    if "@CASES@" in code:
        cases = " ".join(f"case 0x{a:x}: goto L_{a:x};" for a in sorted(tr.labels))
        code = code.replace("@CASES@", cases)
    return code


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("module")
    p.add_argument("--out", required=True)
    p.add_argument("--funcs", help="comma-separated list of function start offsets")
    p.add_argument("--all", action="store_true")
    p.add_argument("--stats", action="store_true")
    p.add_argument("--per-file", type=int, default=2000)
    p.add_argument("--callee-depth", type=int, default=0,
                   help="also translate functions called (directly) by the selection, up to this depth")
    args = p.parse_args(argv[1:])

    mod = analyze.Module("mod", args.module)
    text_end = mod.mod0 & ~0xFFF
    mod.find_text_end(text_end)
    bounds = build_function_list(mod, text_end)
    func_set = set(bounds)

    if args.funcs:
        selected = [int(x, 16) for x in args.funcs.split(",")]
    else:
        selected = sorted(bounds)

    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM)
    md.detail = True
    md.skipdata = True

    os.makedirs(args.out, exist_ok=True)
    unsupported = collections.Counter()
    clean = 0
    fn_clean = {}
    chunks, chunk, decls = [], [], set()
    queue = [(a, 0) for a in selected]
    seen = set(selected)
    selected = []
    while queue:
        start, depth = queue.pop(0)
        selected.append(start)
        tr = FunctionTranslator(mod, start, bounds[start], func_set, md)
        if depth < args.callee_depth:
            for cal in sorted(tr.callees_preview()):
                if cal not in seen:
                    seen.add(cal)
                    queue.append((cal, depth + 1))
        code = finalize(tr.translate(), tr)
        unsupported.update(tr.unsupported)
        fn_clean[start] = not tr.unsupported
        clean += not tr.unsupported
        decls |= tr.callees | {start}
        chunk.append(code)
        if len(chunk) >= args.per_file:
            chunks.append(chunk)
            chunk = []
    if chunk:
        chunks.append(chunk)

    with open(os.path.join(args.out, "funcs.h"), "w") as f:
        f.write("#pragma once\n#include \"recomp_cpu.h\"\n")
        for a in sorted(decls):
            f.write(f"void f_{a:x}(cpu_t *c);\n")
    for n, ch in enumerate(chunks):
        with open(os.path.join(args.out, f"funcs_{n:03d}.c"), "w") as f:
            f.write(PRELUDE + '#include "funcs.h"\n\n' + "\n\n".join(ch) + "\n")
    selected.sort()
    with open(os.path.join(args.out, "table.c"), "w") as f:
        f.write('#include "funcs.h"\n#include <stddef.h>\nextern uint64_t recomp_module_base;\n')
        f.write("typedef struct { uint64_t addr; recomp_fn_t fn; } recomp_entry_t;\n")
        f.write("const recomp_entry_t recomp_table[] = {\n")
        for a in selected:
            f.write(f"    {{0x{a:x}ull, f_{a:x}}},\n")
        f.write("};\nconst size_t recomp_table_size = sizeof(recomp_table) / sizeof(recomp_table[0]);\n")
        stubs = sorted(decls - set(selected))
        for a in stubs:
            f.write(f'void f_{a:x}(cpu_t *c) {{ recomp_dispatch(c, recomp_module_base + 0x{a:x}ull); }}\n')

    if args.stats:
        n = len(selected)
        print(f"functions: {n:,}  fully translated: {clean:,} ({100 * clean / max(n, 1):.1f}%)")
        print("top unsupported mnemonics (instruction count):")
        for m, c in unsupported.most_common(40):
            print(f"  {c:8,}  {m}")
    with open(os.path.join(args.out, "clean.txt"), "w") as f:
        for a, ok in fn_clean.items():
            f.write(f"{a:x} {int(ok)}\n")


if __name__ == "__main__":
    main(sys.argv)
