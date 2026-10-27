#!/usr/bin/env python3
"""SM86 (Switch 2 / T239) shader -> Vulkan GLSL translator.

Input: an NVN2 shader code blob as captured by the host (RECOMP_SHADER_DUMP): a 0x80-byte
header (byte 0: 0x81 vertex, 0x82 fragment; anything else is compute code with no header)
followed by 128-bit instructions. Output: GLSL 460 plus a resource description (JSON).

Translation model
- Registers R0..R254 become `uint r<n>` variables (RZ = 0), predicates P0..P6 `bool p<n>`
  (PT = true), uniform registers UR0..UR62 `uint u<n>` (URZ = 0), uniform predicates `bool up<n>`.
- Control flow is kept as a program-counter loop over basic blocks (`switch (pc)`), which
  handles any branch structure; BSSY/BSYNC (reconvergence hints) are no-ops.
- Constant buffers c[bank][offset] read `cb<bank>` UBOs (uvec4 arrays, std140).
- Textures referenced through a constant-buffer handle c[bank][off] become combined samplers
  `tex_<bank>_<off>`; the renderer resolves the handle at draw time.
- Vertex attributes (ALD/AST) and fragment inputs (IPA) use the Maxwell attribute space:
  0x70..0x7c position, 0x80 + 16 * i generic attribute i.
- Fragment outputs: at EXIT, render target t reads R(4t)..R(4t+3) for the targets enabled in
  the header output mask (header word 0x48).

Usage: translate.py <blob> [-o out.glsl] [--json out.json] [--dis]
"""

import json
import struct
import sys

from isa import ALU, OTHER, bits, opcode

RZ, PT, URZ = 255, 7, 63


class Unsupported(Exception):
    pass


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


def float_lit(u):
    """GLSL expression for the float with bit pattern u (exact)."""
    v = f32(u)
    if v != v or v in (float('inf'), float('-inf')):
        return 'uintBitsToFloat(0x%08xu)' % u
    r = repr(v)
    if 'e' not in r and '.' not in r:
        r += '.0'
    if struct.unpack('<I', struct.pack('<f', float(r)))[0] != u:
        return 'uintBitsToFloat(0x%08xu)' % u
    return r


class Inst:
    def __init__(self, off, v):
        self.off, self.v = off, v
        self.name, self.form = opcode(v)
        self.op12 = v & 0xFFF

    def f(self, lo, hi):
        return bits(self.v, lo, hi)

    def b(self, n):
        return (self.v >> n) & 1


class Translator:
    def __init__(self, blob):
        self.blob = blob
        self.kind = {0x81: 'vertex', 0x82: 'fragment'}.get(blob[0], 'compute')
        self.start = 0x80 if self.kind != 'compute' else 0
        self.hdr = [struct.unpack_from('<I', blob, i)[0] for i in range(0, 0x80, 4)] if self.start else []
        self.insts = []
        self.regs, self.preds, self.uregs, self.upreds = set(), set(), set(), set()
        self.cbufs = {}          # bank -> max byte offset
        self.textures = {}       # (bank, off) -> dim
        self.inputs = set()      # vertex: generic attribute indices read
        self.outputs = set()     # vertex: generic outputs written / fragment: varyings read
        self.uses_position_out = False
        self.frag_inputs = set()
        self.need = set()        # helper functions

    # ---- decoding -------------------------------------------------------------------------
    def decode(self):
        off = self.start
        seen_exit = False
        while off + 16 <= len(self.blob):
            v = int.from_bytes(self.blob[off:off + 16], 'little')
            i = Inst(off, v)
            self.insts.append(i)
            if i.name == 'EXIT':
                seen_exit = True
            elif i.name == 'BRA' and seen_exit and i.f(87, 90) == PT and not i.b(90) and self.bra_target(i) == i.off:
                break
            off += 16
        return self

    def bra_target(self, i):
        rel = i.f(34, 82)
        if rel & (1 << 47):
            rel -= 1 << 48
        return i.off + 16 + rel * 4

    # ---- operand helpers -------------------------------------------------------------------
    def R(self, n):
        if n == RZ:
            return '0u'
        self.regs.add(n)
        return 'r%d' % n

    def Rw(self, n):
        if n == RZ:
            return None
        self.regs.add(n)
        return 'r%d' % n

    def UR(self, n):
        if n == URZ:
            return '0u'
        self.uregs.add(n)
        return 'u%d' % n

    def P(self, n, neg=False):
        if n == PT:
            return 'false' if neg else 'true'
        self.preds.add(n)
        return ('!p%d' if neg else 'p%d') % n

    def Pw(self, n):
        if n == PT:
            return None
        self.preds.add(n)
        return 'p%d' % n

    def UP(self, n, neg=False):
        if n == 7:
            return 'false' if neg else 'true'
        self.upreds.add(n)
        return ('!up%d' if neg else 'up%d') % n

    def cb(self, bank, off, reg=None):
        self.cbufs[bank] = max(self.cbufs.get(bank, 0), off + 4)
        if reg is None:
            return 'cb%d[%d].%s' % (bank, off >> 4, 'xyzw'[(off >> 2) & 3])
        self.need.add('cbread')
        self.cbufs[bank] = max(self.cbufs[bank], 0x10000)
        return 'cbread%d(%s + %du)' % (bank, reg, off)

    def alu_srcs(self, i, uniform=False):
        """(s0, s1, s2) as raw-uint expressions with the form's operand placement."""
        R = self.UR if uniform else self.R
        f = i.form
        s0 = R(i.f(24, 32))
        slot64 = R(i.f(64, 72))
        if f == 1:
            s32 = R(i.f(32, 40)) if not uniform else self.UR(i.f(32, 38))
        elif f in (2, 4):
            s32 = '0x%08xu' % i.f(32, 64)
        elif f in (3, 5):
            if i.b(91):
                raise Unsupported('bindless cbuf operand')
            s32 = self.cb(i.f(54, 59), i.f(38, 54))
        elif f in (6, 7):
            s32 = self.UR(i.f(32, 38))
        else:
            raise Unsupported('form %d' % f)
        if f in (2, 3, 7):
            return s0, slot64, s32, (73, 72), (74, 75), (62, 63)
        return s0, s32, slot64, (73, 72), (62, 63), (74, 75)

    def fsrc(self, i, expr, mods):
        e = 'uintBitsToFloat(%s)' % expr if not expr.startswith('0x') else float_lit(int(expr[:-1], 16))
        if expr == '0u':
            e = '0.0'
        if i.b(mods[0]):
            e = 'abs(%s)' % e
        if i.b(mods[1]):
            e = '-(%s)' % e
        return e

    def isrc(self, i, expr, mods, neg_ok=True):
        e = expr
        if neg_ok and i.b(mods[1]):
            e = '(0u - %s)' % e
        return e

    # ---- translation --------------------------------------------------------------------------
    def translate(self):
        self.decode()
        targets = {self.insts[0].off} if self.insts else set()
        for i in self.insts:
            if i.name == 'BRA':
                targets.add(self.bra_target(i))
                targets.add(i.off + 16)
            elif i.name in ('EXIT', 'KILL', 'CALL', 'RET', 'BRX'):
                targets.add(i.off + 16)
            if i.name == 'CALL':
                targets.add(self.bra_target(i))
        if any(i.name == 'BRX' for i in self.insts):
            targets = {i.off for i in self.insts}
        body = []
        for i in self.insts:
            if i.off in targets:
                body.append('        case 0x%xu:' % i.off)
            code = self.inst(i)
            if code is None:
                continue
            g = i.f(12, 15)
            if g != PT or i.b(15):
                code = 'if (%s) { %s }' % (self.P(g, i.b(15)), code)
            body.append('            ' + code + '   // %s' % i.name)
        return self.emit(body)

    def write(self, dst, expr):
        d = self.Rw(dst)
        return '%s = %s;' % (d, expr) if d else ''

    def inst(self, i):
        n = i.name
        h = getattr(self, 'op_' + n.replace('.', '_'), None)
        if h is None:
            raise Unsupported(n)
        return h(i)

    # -- moves / constants
    def op_MOV(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        return self.write(i.f(16, 24), s1)

    def op_UMOV(self, i):
        if i.form == 4 or (i.v >> 9) & 7 == 4:
            src = '0x%08xu' % i.f(32, 64)
        else:
            src = self.UR(i.f(32, 38))
        u = i.f(16, 22)
        return '' if u == URZ else '%s = %s;' % (self.UR(u), src)

    def op_CS2R(self, i):
        idx = i.f(72, 80)
        if idx != 0xFF:
            raise Unsupported('CS2R sr %x' % idx)
        d = i.f(16, 24)
        out = [self.write(d, '0u')]
        if i.b(80):
            out.append(self.write(d + 1, '0u'))
        return ' '.join(out)

    def op_S2R(self, i):
        idx = i.f(72, 80)
        srs = {0x21: 'gl_LocalInvocationID.x', 0x22: 'gl_LocalInvocationID.y', 0x23: 'gl_LocalInvocationID.z',
               0x25: 'gl_WorkGroupID.x', 0x26: 'gl_WorkGroupID.y', 0x27: 'gl_WorkGroupID.z',
               0x00: '0u', 0x50: '0u'}
        if idx not in srs:
            raise Unsupported('S2R sr %x' % idx)
        return self.write(i.f(16, 24), srs[idx])

    def op_LDC(self, i):
        if i.op12 != 0xb82:
            raise Unsupported('LDC bindless')
        bank, off, reg, mt = i.f(54, 59), i.f(38, 54), i.f(24, 32), i.f(73, 76)
        d = i.f(16, 24)
        if mt in (0, 1, 2, 3):
            word = self.cb(bank, off & ~3, None if reg == RZ else self.R(reg)) if reg == RZ else None
            if word is None:
                raise Unsupported('LDC sub-word indexed')
            sh = (off & 3) * 8
            e = {0: '((%s >> %du) & 0xffu)', 1: 'uint(int(%s << %du) >> 24)', 2: '((%s >> %du) & 0xffffu)',
                 3: 'uint(int(%s << %du) >> 16)'}[mt]
            if mt in (1, 3):
                sh = (24 - sh) if mt == 1 else (16 - sh)
            return self.write(d, e % (word, sh))
        if mt not in (4, 5, 6):
            raise Unsupported('LDC type %d' % mt)
        n = {4: 1, 5: 2, 6: 4}[mt]
        out = []
        for k in range(n):
            if reg == RZ:
                out.append(self.write(d + k, self.cb(bank, off + 4 * k)))
            else:
                out.append(self.write(d + k, self.cb(bank, off + 4 * k, self.R(reg))))
        return ' '.join(x for x in out if x)

    def op_LDCU(self, i):
        bank, off, mt = i.f(54, 59), i.f(38, 54), i.f(73, 76)
        n = {4: 1, 5: 2, 6: 4}.get(mt)
        if n is None:
            raise Unsupported('LDCU type %d' % mt)
        u = self.UR(i.f(24, 30))
        return ' '.join(x for x in (self.write(i.f(16, 24) + k, self.cb(bank, off + 4 * k, u)) for k in range(n)) if x)

    def op_ULDC(self, i):
        if i.op12 not in (0xab9, 0xabb):
            raise Unsupported('ULDC bindless')
        bank, off, mt = i.f(54, 59), i.f(38, 54), i.f(73, 76)
        n = {4: 1, 5: 2, 6: 4}.get(mt)
        if n is None:
            raise Unsupported('ULDC type %d' % mt)
        u = i.f(16, 22)
        return ' '.join('%s = %s;' % (self.UR(u + k), self.cb(bank, off + 4 * k)) for k in range(n) if u + k != URZ)

    # -- float ALU
    def ffmt(self, i, e, sat_bit=77):
        if i.b(sat_bit):
            e = 'clamp(%s, 0.0, 1.0)' % e
        return 'floatBitsToUint(%s)' % e

    def op_FADD(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a = self.fsrc(i, s0, m0)
        b = self.fsrc(i, s1, m1) if i.form in (1, 4, 5, 6) else self.fsrc(i, s2, m2)
        return self.write(i.f(16, 24), self.ffmt(i, '(%s + %s)' % (a, b)))

    def op_FMUL(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a, b = self.fsrc(i, s0, m0), self.fsrc(i, s1, m1)
        e = '(%s * %s)' % (a, b)
        scale = i.f(84, 87)
        if scale != 4:
            e = '(%s * %s)' % (e, {0: '1.0', 1: '0.5', 2: '0.25', 3: '0.125', 5: '2.0', 6: '4.0', 7: '8.0'}[scale])
        return self.write(i.f(16, 24), self.ffmt(i, e))

    def op_FFMA(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        e = 'fma(%s, %s, %s)' % (self.fsrc(i, s0, m0), self.fsrc(i, s1, m1), self.fsrc(i, s2, m2))
        return self.write(i.f(16, 24), self.ffmt(i, e))

    def op_FMNMX(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a, b = self.fsrc(i, s0, m0), self.fsrc(i, s1, m1)
        c = self.P(i.f(87, 90), i.b(90))
        return self.write(i.f(16, 24), 'floatBitsToUint(%s ? min(%s, %s) : max(%s, %s))' % (c, a, b, a, b))

    def op_MUFU(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a = self.fsrc(i, s1, m1)
        ops = {0: 'cos(%s)', 1: 'sin(%s)', 2: 'exp2(%s)', 3: 'log2(%s)', 4: '(1.0 / %s)', 5: 'inversesqrt(%s)',
               8: 'sqrt(%s)', 9: 'tanh(%s)'}
        op = i.f(74, 80)
        if op not in ops:
            raise Unsupported('MUFU op %d' % op)
        return self.write(i.f(16, 24), 'floatBitsToUint(%s)' % (ops[op] % a))

    FCMP = {1: '<', 2: '==', 3: '<=', 4: '>', 5: '!=', 6: '>=', 9: '<', 10: '==', 11: '<=', 12: '>', 13: '!=', 14: '>='}

    def fcmp(self, i, a, b):
        c = i.f(76, 80)
        if c == 0:
            return 'false'
        if c == 15:
            return 'true'
        if c == 7:
            return '(!isnan(%s) && !isnan(%s))' % (a, b)
        if c == 8:
            return '(isnan(%s) || isnan(%s))' % (a, b)
        e = '(%s %s %s)' % (a, self.FCMP[c], b)
        if c >= 9:
            e = '(%s || isnan(%s) || isnan(%s))' % (e, a, b)
        elif c == 5:
            e = '(%s && !isnan(%s) && !isnan(%s))' % (e, a, b)
        return e

    def setp(self, i, cond, dst_lo=81):
        acc = self.P(i.f(87, 90), i.b(90))
        bop = {0: '&&', 1: '||', 2: '!='}[i.f(74, 76)]
        out = []
        d0, d1 = self.Pw(i.f(dst_lo, dst_lo + 3)), self.Pw(i.f(dst_lo + 3, dst_lo + 6))
        if d0:
            out.append('%s = (%s) %s %s;' % (d0, cond, bop, acc))
        if d1:
            out.append('%s = (!(%s)) %s %s;' % (d1, cond, bop, acc))
        return ' '.join(out)

    def op_FSETP(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        return self.setp(i, self.fcmp(i, self.fsrc(i, s0, m0), self.fsrc(i, s1, m1)))

    def op_FSET(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        cond = self.fcmp(i, self.fsrc(i, s0, m0), self.fsrc(i, s1, m1))
        acc = self.P(i.f(87, 90), i.b(90))
        bop = {0: '&&', 1: '||', 2: '!='}[i.f(74, 76)]
        val = '0x3f800000u' if i.b(63 if False else 75) else '0xffffffffu'
        return self.write(i.f(16, 24), '((%s) %s %s) ? %s : 0u' % (cond, bop, acc, val))

    def op_FSEL(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        c = self.P(i.f(87, 90), i.b(90))
        return self.write(i.f(16, 24), '(%s ? %s : %s)' % (c, s0, s1))

    def op_FRND(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a = self.fsrc(i, s1, m1)
        fn = {0: 'roundEven', 1: 'floor', 2: 'ceil', 3: 'trunc'}[i.f(78, 80)]
        return self.write(i.f(16, 24), 'floatBitsToUint(%s(%s))' % (fn, a))

    def op_F2I(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        ds = i.f(75, 77)
        if ds == 3 or i.f(84, 86) != 2:
            raise Unsupported('F2I 64-bit')
        a = self.fsrc(i, s1, m1)
        fn = {0: 'roundEven', 1: 'floor', 2: 'ceil', 3: 'trunc'}[i.f(78, 80)]
        lim = {0: (128.0, 255.0), 1: (32768.0, 65535.0), 2: (2147483648.0, 4294967040.0)}[ds]
        if i.b(72):
            return self.write(i.f(16, 24), 'uint(int(clamp(%s(%s), -%r, %r)))' % (fn, a, lim[0], lim[0] - (128.0 if ds == 2 else 1.0)))
        return self.write(i.f(16, 24), 'uint(clamp(%s(%s), 0.0, %r))' % (fn, a, lim[1]))

    def op_I2F(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        if i.f(75, 77) != 2 or i.f(84, 86) != 2:
            raise Unsupported('I2F size')
        conv = 'float(int(%s))' if i.b(74) else 'float(%s)'
        return self.write(i.f(16, 24), 'floatBitsToUint(%s)' % (conv % s1))

    def op_I2FP(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        conv = 'float(int(%s))' if i.b(74) else 'float(%s)'
        return self.write(i.f(16, 24), 'floatBitsToUint(%s)' % (conv % s1))

    def op_F2F(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        ds, ss = i.f(75, 77), i.f(84, 86)
        a = self.fsrc(i, s1, m1)
        if ds == 2 and ss == 2:
            fn = {0: '', 1: 'floor', 2: 'ceil', 3: 'trunc'}[i.f(78, 80)]
            return self.write(i.f(16, 24), 'floatBitsToUint(%s(%s))' % (fn, a))
        if ds == 1 and ss == 2:
            return self.write(i.f(16, 24), 'packHalf2x16(vec2(%s, 0.0))' % a)
        if ds == 2 and ss == 1:
            return self.write(i.f(16, 24), 'floatBitsToUint(unpackHalf2x16(%s).x)' % s1)
        raise Unsupported('F2F %d<-%d' % (ds, ss))

    # -- integer ALU
    def op_IADD3(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        a, b, c = self.isrc(i, s0, m0), self.isrc(i, s1, m1), self.isrc(i, s2, m2)
        if i.b(74):  # .X: add carry-in predicates
            c = '%s + (%s ? 1u : 0u)' % (c, self.P(i.f(87, 90), i.b(90)))
        out = [self.write(i.f(16, 24), '(%s + %s + %s)' % (a, b, c))]
        if self.Pw(i.f(81, 84)):
            out.append('%s = (uint64_t(%s) + uint64_t(%s) + uint64_t(%s)) > 0xffffffffUL;' % (self.Pw(i.f(81, 84)), a, b, c))
            self.need.add('int64')
        return ' '.join(x for x in out if x)

    def op_LOP3(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        lut = i.f(72, 80)
        e = self.lop3(lut, s0, s1, s2)
        out = [self.write(i.f(16, 24), e)]
        p = self.Pw(i.f(81, 84))
        if p:
            out.append('%s = (%s) != 0u;' % (p, e))
        return ' '.join(x for x in out if x)

    @staticmethod
    def lop3(lut, a, b, c):
        common = {0xc0: '(%s & %s)' % (a, b), 0xfc: '(%s | %s)' % (a, b), 0x3c: '(%s ^ %s)' % (a, b),
                  0xa0: '(%s & %s)' % (a, c), 0xfa: '(%s | %s)' % (a, c), 0x5a: '(%s ^ %s)' % (a, c),
                  0x88: '(%s & %s)' % (b, c), 0xee: '(%s | %s)' % (b, c), 0x66: '(%s ^ %s)' % (b, c),
                  0x30: '(%s & ~%s)' % (a, b), 0x0c: '(~%s & %s)' % (a, b), 0x33: '(~%s)' % b, 0x0f: '(~%s)' % a,
                  0x55: '(~%s)' % c, 0x80: '(%s & %s & %s)' % (a, b, c), 0xfe: '(%s | %s | %s)' % (a, b, c),
                  0x96: '(%s ^ %s ^ %s)' % (a, b, c), 0xf0: a, 0xcc: b, 0xaa: c, 0x00: '0u', 0xff: '0xffffffffu',
                  0xf8: '(%s | (%s & %s))' % (a, b, c), 0xec: '(%s | (%s & %s))' % (b, a, c),
                  0xea: '(%s | (%s & %s))' % (c, a, b), 0x40: '(%s & %s & ~%s)' % (a, b, c),
                  0x20: '(%s & ~%s & %s)' % (a, b, c), 0x08: '(~%s & %s & %s)' % (a, b, c),
                  0x3f: '(~(%s & %s))' % (a, b), 0xe2: '((%s & %s) | (~%s & %s))' % (c, b, b, a)}
        if lut in common:
            return common[lut]
        terms = []
        for k in range(8):
            if lut >> k & 1:
                terms.append('(%s%s & %s%s & %s%s)' % ('' if k & 4 else '~', a, '' if k & 2 else '~', b,
                                                     '' if k & 1 else '~', c))
        return '(' + ' | '.join(terms) + ')'

    def op_ISETP(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        if i.b(72):
            raise Unsupported('ISETP.EX')
        signed = i.b(73)
        a, b = (('int(%s)' % s0, 'int(%s)' % s1) if signed else (s0, s1))
        c = i.f(76, 79)
        cond = {0: 'false', 7: 'true', 1: '(%s < %s)', 2: '(%s == %s)', 3: '(%s <= %s)', 4: '(%s > %s)',
                5: '(%s != %s)', 6: '(%s >= %s)'}[c]
        if '%' in cond:
            cond = cond % (a, b)
        return self.setp(i, cond)

    def op_IMNMX(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        c = self.P(i.f(87, 90), i.b(90))
        if i.b(73):
            e = 'uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))' % (c, s0, s1, s0, s1)
        else:
            e = '(%s ? min(%s, %s) : max(%s, %s))' % (c, s0, s1, s0, s1)
        return self.write(i.f(16, 24), e)

    def op_IABS(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        return self.write(i.f(16, 24), 'uint(abs(int(%s)))' % s1)

    def op_SEL(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        c = self.P(i.f(87, 90), i.b(90))
        return self.write(i.f(16, 24), '(%s ? %s : %s)' % (c, s0, s1))

    def op_SHF(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)   # low, shift, high
        typ, wrap, right, hi = i.f(73, 75), i.b(75), i.b(76), i.b(80)
        sh = '(%s & 63u)' % s1 if wrap else 'min(%s, 64u)' % s1
        if typ in (2, 3) and not hi:
            # 32-bit shift of the low word (with the high word shifted in for right shifts)
            sh32 = '(%s & 31u)' % s1 if wrap else 'min(%s, 32u)' % s1
            self.need.add('shf')
            if right:
                return self.write(i.f(16, 24), 'shf_r(%s, %s, %s, %s)' % (s0, s2, sh32, 'true' if typ == 2 else 'false'))
            return self.write(i.f(16, 24), 'shf_l(%s, %s, %s)' % (s0, s2, sh32))
        self.need.add('int64')
        v = 'packUint2x32(uvec2(%s, %s))' % (s0, s2)
        if typ == 0:
            v = 'int64_t(%s)' % v
        e = '(%s %s %s)' % (v, '>>' if right else '<<', sh)
        part = 'y' if hi else 'x'
        return self.write(i.f(16, 24), 'unpackUint2x32(uint64_t(%s)).%s' % (e, part))

    def op_IMAD(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        c = self.isrc(i, s2, m2)
        return self.write(i.f(16, 24), '(%s * %s + %s)' % (s0, s1, c))

    def op_IMAD_WIDE(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i)
        d = i.f(16, 24)
        self.need.add('int64')
        if i.b(73):
            e = 'uint64_t(int64_t(int(%s)) * int64_t(int(%s)))' % (s0, s1)
        else:
            e = '(uint64_t(%s) * uint64_t(%s))' % (s0, s1)
        c = 'packUint2x32(uvec2(%s, %s))' % (s2, self.R(i.f(64, 72) + 1) if i.form in (1, 4, 5, 6) and i.f(64, 72) != RZ else '0u')
        tmp = 'unpackUint2x32(%s + %s)' % (e, c)
        return ' '.join(x for x in [self.write(d, tmp + '.x'), self.write(d + 1, tmp + '.y')] if x)

    def op_LEA(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        sh, hi = i.f(75, 80), i.b(80)
        if i.b(74):
            raise Unsupported('LEA.X')
        if hi:
            self.need.add('int64')
            v = '(packUint2x32(uvec2(%s, %s)) << %d)' % (s0, s2, sh)
            return self.write(i.f(16, 24), '(unpackUint2x32(%s).y + %s)' % (v, s1))
        a = '(0u - %s)' % s0 if i.b(72) else s0
        return self.write(i.f(16, 24), '((%s << %du) + %s)' % (a, sh, s1))

    def op_PRMT(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        if i.f(72, 75) != 0:
            raise Unsupported('PRMT mode')
        self.need.add('prmt')
        return self.write(i.f(16, 24), 'prmt(%s, %s, %s)' % (s0, s1, s2))

    def op_POPC(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        x = '(~%s)' % s1 if i.b(63) else s1
        return self.write(i.f(16, 24), 'uint(bitCount(%s))' % x)

    def op_FLO(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        x = '(~%s)' % s1 if i.b(63) else s1
        e = 'uint(findMSB(%s(%s)))' % ('int' if i.b(73) else 'uint', x)
        if i.b(74):
            e = '(31u - %s)' % e
        return self.write(i.f(16, 24), e)

    def op_BREV(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        return self.write(i.f(16, 24), 'bitfieldReverse(%s)' % s1)

    def op_BMSK(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        w = '(%s & 31u)' % s1 if i.b(75) else 'min(%s, 32u)' % s1
        return self.write(i.f(16, 24), '(((%s == 32u) ? 0xffffffffu : ((1u << %s) - 1u)) << (%s & 31u))' % (w, w, s0))

    def op_SGXT(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        if i.b(73):
            return self.write(i.f(16, 24), 'uint(bitfieldExtract(int(%s), 0, int(min(%s, 32u))))' % (s0, s1))
        return self.write(i.f(16, 24), 'bitfieldExtract(%s, 0, int(min(%s, 32u)))' % (s0, s1))

    # -- predicates
    def op_PLOP3(self, i):
        if i.b(67):
            raise Unsupported('PLOP3 upred')
        a = self.P(i.f(87, 90), i.b(90))
        b = self.P(i.f(77, 80), i.b(80))
        c = self.P(i.f(68, 71), i.b(71))
        lut0 = i.f(64, 67) | (i.f(72, 77) << 3)
        lut1 = i.f(16, 24)
        out = []
        for lut, d in ((lut0, i.f(81, 84)), (lut1, i.f(84, 87))):
            dw = self.Pw(d)
            if dw:
                e = self.lop3(lut, '(%s ? 0xffffffffu : 0u)' % a, '(%s ? 0xffffffffu : 0u)' % b,
                              '(%s ? 0xffffffffu : 0u)' % c)
                out.append('%s = (%s) != 0u;' % (dw, e))
        return ' '.join(out)

    def op_R2P(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        out = []
        for k in range(7):
            out.append('if ((%s & %du) != 0u) %s = (%s & %du) != 0u;' % (s1, 1 << k, self.P(k), s0, 1 << k))
        return ' '.join(out)

    def op_P2R(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i)
        parts = ' | '.join('(%s ? %du : 0u)' % (self.P(k), 1 << k) for k in range(7))
        return self.write(i.f(16, 24), '((%s) & %s) | (%s & ~%s)' % (parts, s1, s0, s1))

    # -- uniform datapath: same encodings on uniform registers
    def uwrite(self, i, expr):
        u = i.f(16, 22)
        return '' if u == URZ else '%s = %s;' % (self.UR(u), expr)

    def op_UIADD3(self, i):
        s0, s1, s2, m0, m1, m2 = self.alu_srcs(i, uniform=True)
        return self.uwrite(i, '(%s + %s + %s)' % (self.isrc(i, s0, m0), self.isrc(i, s1, m1), self.isrc(i, s2, m2)))

    def op_UIMAD(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        return self.uwrite(i, '(%s * %s + %s)' % (s0, s1, s2))

    def op_ULOP3(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        return self.uwrite(i, self.lop3(i.f(72, 80), s0, s1, s2))

    def op_USHF(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        typ, right = i.f(73, 75), i.b(76)
        if typ not in (2, 3) or i.b(80):
            raise Unsupported('USHF 64')
        return self.uwrite(i, '(%s %s min(%s, 31u))' % (s0, '>>' if right else '<<', s1))

    def op_UISETP(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        signed = i.b(73)
        a, b = (('int(%s)' % s0, 'int(%s)' % s1) if signed else (s0, s1))
        cond = {0: 'false', 7: 'true', 1: '(%s < %s)', 2: '(%s == %s)', 3: '(%s <= %s)', 4: '(%s > %s)',
                5: '(%s != %s)', 6: '(%s >= %s)'}[i.f(76, 79)]
        if '%' in cond:
            cond = cond % (a, b)
        acc = self.UP(i.f(87, 90), i.b(90))
        bop = {0: '&&', 1: '||', 2: '!='}[i.f(74, 76)]
        d = i.f(81, 84)
        return '' if d == 7 else '%s = (%s) %s %s;' % (self.UP(d), cond, bop, acc)

    def op_UFLO(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        return self.uwrite(i, 'uint(findMSB(%s))' % s1)

    def op_UPOPC(self, i):
        s0, s1, s2, *_ = self.alu_srcs(i, uniform=True)
        return self.uwrite(i, 'uint(bitCount(%s))' % s1)

    def op_R2UR(self, i):
        u = i.f(16, 22)
        return '' if u == URZ else '%s = %s;' % (self.UR(u), self.R(i.f(24, 32)))

    def op_VOTEU(self, i):
        u = i.f(16, 22)
        return '' if u == URZ else '%s = 1u;' % self.UR(u)   # single-lane execution model

    def op_VOTE(self, i):
        p = self.P(i.f(87, 90), i.b(90))
        out = [self.write(i.f(16, 24), '(%s ? 1u : 0u)' % p)]
        d = self.Pw(i.f(81, 84))
        if d:
            out.append('%s = %s;' % (d, p))
        return ' '.join(x for x in out if x)

    # -- attributes / varyings
    def attr_name(self, addr, store):
        if 0x70 <= addr < 0x80:
            comp = 'xyzw'[(addr - 0x70) >> 2]
            if self.kind == 'vertex' and store:
                self.uses_position_out = True
                return 'gl_Position.%s' % comp
            if self.kind == 'fragment':
                return 'gl_FragCoord.%s' % comp
        sysvals = {0x2f8: ('uint(gl_InstanceIndex)', True), 0x2fc: ('uint(gl_VertexIndex)', True),
                   0x3fc: ('(gl_FrontFacing ? 0xffffffffu : 0u)', True), 0x6c: ('1.0', False)}
        if addr in sysvals and not store:
            e, is_uint = sysvals[addr]
            return ('U:' + e) if is_uint else e
        if 0x80 <= addr < 0x280:
            idx, comp = (addr - 0x80) >> 4, 'xyzw'[((addr - 0x80) >> 2) & 3]
            if self.kind == 'vertex':
                if store:
                    self.outputs.add(idx)
                    return 'v%d.%s' % (idx, comp)
                self.inputs.add(idx)
                return 'a%d.%s' % (idx, comp)
            self.frag_inputs.add(idx)
            return 'v%d.%s' % (idx, comp)
        raise Unsupported('attribute 0x%x' % addr)

    def op_ALD(self, i):
        if i.f(24, 32) != RZ or i.b(79) or i.b(76):
            raise Unsupported('ALD indexed/output/patch')
        addr, n, d = i.f(40, 50), i.f(74, 76) + 1, i.f(16, 24)
        out = []
        for k in range(n):
            a = self.attr_name(addr + 4 * k, False)
            out.append(self.write(d + k, a[2:] if a.startswith('U:') else 'floatBitsToUint(%s)' % a))
        return ' '.join(x for x in out if x)

    def op_AST(self, i):
        if i.f(24, 32) != RZ or i.b(76):
            raise Unsupported('AST indexed/patch')
        addr, n, s = i.f(40, 50), i.f(74, 76) + 1, i.f(32, 40)
        return ' '.join('%s = uintBitsToFloat(%s);' % (self.attr_name(addr + 4 * k, True), self.R(s + k)) for k in range(n))

    def op_IPA(self, i):
        addr = i.f(64, 72) << 2
        if addr == 0x7c:
            val = '(1.0 / gl_FragCoord.w)'
        else:
            val = self.attr_name(addr, False)
        if val.startswith('U:'):
            return self.write(i.f(16, 24), val[2:])
        return self.write(i.f(16, 24), 'floatBitsToUint(%s)' % val)

    # -- global memory: one storage buffer per draw, based at gmem_base (set by the renderer)
    def gaddr(self, i, ureg_lo=32, ureg_bit=72):
        self.need.add('int64')
        self.need.add('gmem')
        r = i.f(24, 32)
        lo = self.R(r)
        hi = self.R(r + 1) if (i.b(90) and r != RZ) else '0u'
        addr = 'packUint2x32(uvec2(%s, %s))' % (lo, hi)
        if i.b(91):
            u = i.f(ureg_lo, ureg_lo + 6)
            if u != URZ:
                uhi = self.UR(u + 1) if i.b(ureg_bit) else '0u'
                addr = '(%s + packUint2x32(uvec2(%s, %s)))' % (addr, self.UR(u), uhi)
        off = i.f(40, 64)
        if off & (1 << 23):
            off -= 1 << 24
        return 'gidx(%s + uint64_t(int64_t(%d)))' % (addr, off)

    def op_LDG(self, i):
        if i.op12 != 0x981:
            raise Unsupported('LDG form')
        mt, d = i.f(73, 76), i.f(16, 24)
        a = self.gaddr(i)
        if mt in (4, 5, 6):
            n = {4: 1, 5: 2, 6: 4}[mt]
            return 'uint ga%x = %s; %s' % (i.off, a, ' '.join(x for x in (self.write(d + k, 'gmem.w[ga%x + %du]' % (i.off, k)) for k in range(n)) if x))
        self.need.add('gbyte')
        e = {0: 'gload8(%s)', 1: 'uint(int(gload8(%s) << 24) >> 24)', 2: 'gload16(%s)', 3: 'uint(int(gload16(%s) << 16) >> 16)'}[mt]
        return self.write(d, e % a.replace('gidx(', 'gbyteaddr('))

    def op_STG(self, i):
        if i.op12 != 0x986:
            raise Unsupported('STG form')
        mt, s = i.f(73, 76), i.f(32, 40)
        if mt not in (4, 5, 6):
            raise Unsupported('STG type %d' % mt)
        self.gmem_writes = True
        a = self.gaddr(i, ureg_lo=64)
        n = {4: 1, 5: 2, 6: 4}[mt]
        return 'uint ga%x = %s; %s' % (i.off, a, ' '.join('gmem.w[ga%x + %du] = %s;' % (i.off, k, self.R(s + k)) for k in range(n)))

    # -- textures
    def tex_common(self, i, kind):
        if i.op12 not in (0xb60, 0xb66, 0xb63, 0xb69):
            raise Unsupported('%s bindless' % kind)
        off, bank = i.f(40, 54) * 4, i.f(54, 59)
        dim = i.f(61, 64)
        dims = {0: ('sampler1D', 1), 1: ('sampler2D', 2), 2: ('sampler3D', 3), 3: ('samplerCube', 3),
                4: ('sampler1DArray', 1), 5: ('sampler2DArray', 2), 7: ('samplerCubeArray', 3)}
        if dim not in dims:
            raise Unsupported('tex dim %d' % dim)
        stype, ncoord = dims[dim]
        shadow = kind == 'TEX' and i.b(78)
        if shadow:
            stype += 'Shadow'
        name = 'tex_%d_%x' % (bank, off)
        prev = self.textures.get((bank, off))
        if prev and prev != stype:
            raise Unsupported('texture used with two types')
        self.textures[(bank, off)] = stype
        array = dim in (4, 5, 7)
        return name, stype, ncoord, array, shadow

    def tex_srcs(self, i, nlogical0, nlogical1, scalar):
        """Logical source lists (src0 list, src1 list) as register expressions."""
        r0, r1 = i.f(24, 32), i.f(32, 40)
        if scalar:
            total = nlogical0 + nlogical1
            split = {1: (1, 0), 2: (1, 1), 3: (2, 1), 4: (2, 2)}[total]
            regs = [r0 + k for k in range(split[0])] + [r1 + k for k in range(split[1])]
            vals = [self.R(r) if r != RZ else '0u' for r in regs]
            return vals[:nlogical0], vals[nlogical0:]
        a = [self.R(r0 + k) if r0 != RZ else '0u' for k in range(nlogical0)]
        b = [self.R(r1 + k) if r1 != RZ else '0u' for k in range(nlogical1)]
        return a, b

    def tex_dsts(self, i, values):
        mask = i.f(72, 76)
        comps = [c for c in range(4) if mask >> c & 1]
        d0, d1 = i.f(16, 24), i.f(64, 72)
        out = []
        for n, c in enumerate(comps):
            reg = (d0 + n) if n < 2 else (d1 + n - 2 if d1 != RZ else RZ)
            if n >= 2 and d1 == RZ:
                continue
            w = self.write(reg, values[c])
            if w:
                out.append(w)
        return out

    def op_TEX(self, i):
        name, stype, ncoord, array, shadow = self.tex_common(i, 'TEX')
        lod_mode = i.f(87, 90)
        if i.b(76):
            raise Unsupported('TEX offset')
        n0 = ncoord + (1 if array else 0)
        n1 = (1 if lod_mode in (2, 3) else 0) + (1 if shadow else 0)
        src0, src1 = self.tex_srcs(i, n0, n1, i.b(60))
        f = ['uintBitsToFloat(%s)' % s for s in src0]
        if array:
            layer = 'float(%s)' % src0[0]
            coords = f[1:] + [layer]
        else:
            coords = f
        k = 0
        lod = None
        if lod_mode in (2, 3):
            lod = 'uintBitsToFloat(%s)' % src1[k]
            k += 1
        if shadow:
            coords = coords + ['uintBitsToFloat(%s)' % src1[k]]
        vec = coords[0] if len(coords) == 1 else 'vec%d(%s)' % (len(coords), ', '.join(coords))
        if lod_mode == 0:
            call = 'texture(%s, %s)' % (name, vec)
        elif lod_mode == 1:
            call = 'textureLod(%s, %s, 0.0)' % (name, vec)
        elif lod_mode == 2:
            call = 'texture(%s, %s, %s)' % (name, vec, lod)
        elif lod_mode == 3:
            call = 'textureLod(%s, %s, %s)' % (name, vec, lod)
        else:
            raise Unsupported('TEX lod mode %d' % lod_mode)
        if self.kind != 'fragment' and lod_mode in (0, 2):
            call = call.replace('texture(', 'textureLod(').replace(')', ', 0.0)', 1) if lod_mode == 0 else call
        if shadow:
            call = 'vec4(%s)' % call
        t = 'tv%x' % i.off
        vals = ['floatBitsToUint(%s.%s)' % (t, c) for c in 'xyzw']
        return 'vec4 %s = %s; %s' % (t, call, ' '.join(self.tex_dsts(i, vals)))

    def op_TLD(self, i):
        name, stype, ncoord, array, shadow = self.tex_common(i, 'TLD')
        lod_mode = i.f(87, 90)
        if i.b(76) or i.b(78):
            raise Unsupported('TLD offset/ms')
        n0 = ncoord + (1 if array else 0)
        n1 = 1 if lod_mode == 3 else 0
        src0, src1 = self.tex_srcs(i, n0, n1, i.b(60))
        ints = ['int(%s)' % s for s in src0]
        if array:
            ints = ints[1:] + [ints[0]]
        vec = ints[0] if len(ints) == 1 else 'ivec%d(%s)' % (len(ints), ', '.join(ints))
        lod = 'int(%s)' % src1[0] if lod_mode == 3 else '0'
        t = 'tv%x' % i.off
        vals = ['floatBitsToUint(%s.%s)' % (t, c) for c in 'xyzw']
        return 'vec4 %s = texelFetch(%s, %s, %s); %s' % (t, name, vec, lod, ' '.join(self.tex_dsts(i, vals)))

    def op_TXQ(self, i):
        if i.op12 != 0xb6f:
            raise Unsupported('TXQ bindless')
        off, bank = i.f(40, 54) * 4, i.f(54, 59)
        name = 'tex_%d_%x' % (bank, off)
        stype = self.textures.setdefault((bank, off), 'sampler2D')
        if i.f(62, 64) != 0:
            raise Unsupported('TXQ query')
        lod = self.R(i.f(24, 32))
        t = 'tq%x' % i.off
        vals = ['%s.x' % t, '%s.y' % t, '0u', 'uint(textureQueryLevels(%s))' % name]
        return 'uvec2 %s = uvec2(textureSize(%s, int(%s)).xy); %s' % (t, name, lod, ' '.join(self.tex_dsts(i, vals)))

    # -- control flow
    def op_BRA(self, i):
        if i.op12 != 0x947:
            raise Unsupported('BRA.U')
        cond = self.P(i.f(87, 90), i.b(90))
        t = self.bra_target(i)
        if cond == 'true':
            return 'pc = 0x%xu; break;' % t
        return 'if (%s) { pc = 0x%xu; break; }' % (cond, t)

    def op_BRX(self, i):
        self.dynamic_pc = True
        rel = i.f(34, 82)
        if rel & (1 << 47):
            rel -= 1 << 48
        return 'pc = uint(%d + int(%s)); break;' % (i.off + 16 + rel * 4, self.R(i.f(24, 32)))

    def op_CALL(self, i):
        self.uses_calls = True
        return 'cstack[csp++] = 0x%xu; pc = 0x%xu; break;' % (i.off + 16, self.bra_target(i))

    def op_RET(self, i):
        self.uses_calls = True
        return 'pc = cstack[--csp]; break;'

    def op_EXIT(self, i):
        cond = self.P(i.f(87, 90), i.b(90))
        return ('return;' if cond == 'true' else 'if (%s) return;' % cond)

    def op_KILL(self, i):
        cond = self.P(i.f(87, 90), i.b(90))
        return 'discard;' if cond == 'true' else 'if (%s) discard;' % cond

    def op_BSSY(self, i):
        return None

    def op_BSYNC(self, i):
        return None

    def op_NOP(self, i):
        return None

    def op_DEPBAR(self, i):
        return None

    def op_WARPSYNC(self, i):
        return None

    # ---- output ---------------------------------------------------------------------------------
    def frag_targets(self):
        mask = self.hdr[0x48 // 4] if self.hdr else 0xF
        return [t for t in range(8) if (mask >> (4 * t)) & 0xF]

    def emit(self, body):
        L = ['#version 460', '#extension GL_EXT_shader_explicit_arithmetic_types_int64 : enable'
             if 'int64' in self.need else '']
        stage_set = {'vertex': 0, 'fragment': 1, 'compute': 0}[self.kind]
        for bank, size in sorted(self.cbufs.items()):
            n = min(4096, (size + 15) // 16)
            L.append('layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%d]; };' % (stage_set, bank, bank, bank, n))
            if 'cbread' in self.need:
                L.append('uint cbread%d(uint o) { o = min(o, %du); return cb%d[o >> 4][(o >> 2) & 3u]; }' % (bank, n * 16 - 4, bank))
        for k, ((bank, off), stype) in enumerate(sorted(self.textures.items())):
            L.append('layout(set = %d, binding = %d) uniform %s tex_%d_%x;' % (stage_set, 32 + k, stype, bank, off))
        if self.kind == 'vertex':
            for a in sorted(self.inputs):
                L.append('layout(location = %d) in vec4 a%d;' % (a, a))
            for o in sorted(self.outputs):
                L.append('layout(location = %d) out vec4 v%d;' % (o, o))
        elif self.kind == 'fragment':
            for o in sorted(self.frag_inputs):
                L.append('layout(location = %d) in vec4 v%d;' % (o, o))
            for t in self.frag_targets():
                L.append('layout(location = %d) out vec4 o%d;' % (t, t))
        else:
            L.append('layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;')
        if 'gmem' in self.need:
            ro = '' if getattr(self, 'gmem_writes', False) else 'readonly '
            L.append('layout(std430, set = %d, binding = 17) %sbuffer GMem { uint w[]; } gmem;' % (stage_set, ro))
            L.append('layout(push_constant) uniform Push { uint64_t gmem_base; } push;')
            L.append('uint gidx(uint64_t a) { return uint((a - push.gmem_base) >> 2); }')
            if 'gbyte' in self.need:
                L.append('uint64_t gbyteaddr(uint64_t a) { return a - push.gmem_base; }')
                L.append('uint gload8(uint64_t o) { return (gmem.w[uint(o >> 2)] >> (8u * uint(o & 3ul))) & 0xffu; }')
                L.append('uint gload16(uint64_t o) { return (gmem.w[uint(o >> 2)] >> (8u * uint(o & 2ul))) & 0xffffu; }')
        if 'shf' in self.need:
            L.append('uint shf_r(uint lo, uint hi, uint s, bool sgn) { if (s == 0u) return lo; '
                     'return s >= 32u ? (sgn ? uint(int(hi) >> int(s - 32u)) : (hi >> (s - 32u))) : ((lo >> s) | (hi << (32u - s))); }')
            L.append('uint shf_l(uint lo, uint hi, uint s) { return s >= 32u ? 0u : (lo << s); }')
        if 'prmt' in self.need:
            L.append('uint prmt(uint a, uint sel, uint b) { uint r = 0u; for (int k = 0; k < 4; ++k) { uint s = (sel >> (4 * k)) & 7u; '
                     'uint byte_ = s < 4u ? (a >> (8u * s)) & 0xffu : (b >> (8u * (s - 4u))) & 0xffu; '
                     'if (((sel >> (4 * k)) & 8u) != 0u) byte_ = (byte_ & 0x80u) != 0u ? 0xffu : 0u; r |= byte_ << (8 * k); } return r; }')
        if self.kind == 'fragment':
            for t in self.frag_targets():
                for c in range(4):
                    self.regs.add(4 * t + c)
        L.append('')
        for r in sorted(self.regs):
            L.append('uint r%d = 0u;' % r)
        for p in sorted(self.preds):
            L.append('bool p%d = false;' % p)
        for u in sorted(self.uregs):
            L.append('uint u%d = 0u;' % u)
        for p in sorted(self.upreds):
            L.append('bool up%d = false;' % p)
        if self.kind == 'fragment':
            L.append('void store_outputs() {')
            for t in self.frag_targets():
                L.append('    o%d = vec4(%s);' % (t, ', '.join('uintBitsToFloat(r%d)' % (4 * t + c) for c in range(4))))
            L.append('}')
        if self.kind == 'vertex':
            # NVN's default depth mode maps clip z from -w..w (OpenGL); Vulkan uses 0..w. The
            # renderer turns the conversion off (constant 10 = false) for NEAR_IS_ZERO.
            L.append('layout(constant_id = 10) const bool DEPTH_MINUS_ONE = true;')
            L.append('void vmain();')
            L.append('void main() {')
            L.append('    vmain();')
            L.append('    if (DEPTH_MINUS_ONE) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;')
            L.append('}')
        L.append('void vmain() {' if self.kind == 'vertex' else 'void main() {')
        if self.kind == 'vertex':
            L.append('    gl_Position = vec4(0.0);')
        L.append('    uint pc = 0x%xu;' % (self.insts[0].off if self.insts else 0))
        if getattr(self, 'uses_calls', False):
            L.append('    uint cstack[8]; int csp = 0;')
        L.append('    for (int guard = 0; guard < 1000000; ++guard) {')
        L.append('        switch (pc) {')
        out = []
        for line in body:
            if self.kind == 'fragment':
                line = line.replace('return;', '{ store_outputs(); return; }')
            out.append('    ' + line)
        L.extend(out)
        L.append('            return;')
        L.append('        default: return;')
        L.append('        }')
        L.append('    }')
        L.append('}')
        return '\n'.join(x for x in L if x is not None)

    def resources(self):
        return {
            'stage': self.kind,
            'cbufs': {str(k): v for k, v in sorted(self.cbufs.items())},
            'textures': [{'bank': b, 'offset': o, 'type': t, 'binding': 32 + k}
                         for k, ((b, o), t) in enumerate(sorted(self.textures.items()))],
            'inputs': sorted(self.inputs if self.kind == 'vertex' else self.frag_inputs),
            'outputs': sorted(self.outputs) if self.kind == 'vertex' else self.frag_targets(),
            'gmem': 'gmem' in self.need,
        }

    def resources_text(self):
        """The resource description in the line format the host renderer reads (<hash>.res)."""
        r = self.resources()
        L = ['stage %s' % r['stage']]
        L += ['cbuf %s %d' % (b, n) for b, n in r['cbufs'].items()]
        L += ['tex %d %d %s %d' % (t['bank'], t['offset'], t['type'], t['binding']) for t in r['textures']]
        L += ['in %d' % a for a in r['inputs']]
        L += ['out %d' % o for o in r['outputs']]
        if r['gmem']:
            L.append('gmem')
        return '\n'.join(L) + '\n'


def main(argv):
    path = argv[1]
    out = json_out = None
    dis = '--dis' in argv
    if '-o' in argv:
        out = argv[argv.index('-o') + 1]
    if '--json' in argv:
        json_out = argv[argv.index('--json') + 1]
    t = Translator(open(path, 'rb').read())
    if dis:
        t.decode()
        for i in t.insts:
            print('%5x %-10s %032x' % (i.off, i.name + ('' if i.form is None else '.%d' % i.form), i.v))
        return 0
    glsl = t.translate()
    if out:
        open(out, 'w').write(glsl)
    else:
        print(glsl)
    if json_out:
        json.dump(t.resources(), open(json_out, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
