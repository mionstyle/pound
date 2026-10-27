#!/usr/bin/env python3
"""Disassembles code around module offsets, for reading the game's callers of HLE procs.

disasm.py <module.bin> <offset> [<offset> ...] [--before N] [--after N]

Each offset (hex, e.g. 0x320710 as printed by addr_describe "main+0x320710") is shown with
N instructions before / after it (default 40 / 12), the enclosing .eh_frame function start
when known, and the nearest exported symbol. BL targets are annotated with symbol names.
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import Module  # noqa: E402
import capstone as cs  # noqa: E402


def main(argv):
    path = argv[1]
    before = int(argv[argv.index('--before') + 1]) if '--before' in argv else 40
    after = int(argv[argv.index('--after') + 1]) if '--after' in argv else 12
    offs = [int(a, 16) for a in argv[2:] if a.startswith('0x')]
    m = Module(os.path.basename(path), path)
    m.parse_dynamic()
    m.parse_symbols()
    names = sorted((s['value'], s['name']) for s in m.exports if s['value'])
    addrs = [a for a, _ in names]
    try:
        funcs = sorted(m.eh_frame_functions())
    except Exception:
        funcs = []
    starts = [f[0] if isinstance(f, tuple) else f for f in funcs]

    def sym(a):
        i = bisect.bisect_right(addrs, a) - 1
        return '%s+0x%x' % (names[i][1], a - names[i][0]) if i >= 0 else '?'

    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_ARM)
    for off in offs:
        i = bisect.bisect_right(starts, off) - 1
        fstart = starts[i] if i >= 0 else None
        lo = max(off - 4 * before, fstart if fstart is not None and off - fstart < 4 * 400 else 0)
        print('=== 0x%x (function start %s, nearest symbol %s)' % (
            off, hex(fstart) if fstart is not None else '?', sym(off)))
        for ins in md.disasm(m.d[lo:off + 4 * after], lo):
            note = ''
            if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
                note = '    ; ' + sym(int(ins.op_str[1:], 16))
            mark = '>>' if ins.address == off - 4 else '  '
            print('%s %8x: %-8s %s%s' % (mark, ins.address, ins.mnemonic, ins.op_str, note))


if __name__ == '__main__':
    main(sys.argv)
