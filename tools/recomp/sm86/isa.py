"""SM86 (Ampere / Switch 2 T239) instruction set: opcode table and field helpers.

Instructions are 128 bits, little endian. Bits 0..11 hold the opcode. For ALU instructions
the opcode is bits 0..8 and bits 9..11 select the operand form:
  1 = reg, reg, reg     2 = reg, reg, imm32 (src2 imm)    3 = reg, reg, cbuf (src2 cbuf)
  4 = reg, imm32, reg   5 = reg, cbuf, reg                6 = reg, ureg, reg   7 = reg, reg, ureg
Common fields: pred guard 12..14 (+15 not), dst 16..23, src0 24..31, src1 32..39 (or imm 32..63,
cbuf: offset 40..53 (x4), bank 54..58), src2 64..71. Scheduling: 105..125.
The field layout follows the public description of the SM70+ encoding in Mesa's NAK compiler.
"""

ALU = {  # 9-bit opcode -> mnemonic (forms 1..7)
    0x002: 'MOV', 0x007: 'SEL', 0x009: 'FMNMX', 0x00a: 'FSET', 0x00b: 'FSETP', 0x00c: 'ISETP',
    0x010: 'IADD3', 0x011: 'LEA', 0x012: 'LOP3', 0x013: 'IABS', 0x016: 'PRMT', 0x017: 'IMNMX',
    0x019: 'SHF', 0x01a: 'SGXT', 0x01b: 'BMSK', 0x020: 'FMUL', 0x021: 'FADD', 0x023: 'FFMA',
    0x024: 'IMAD', 0x025: 'IMAD.WIDE', 0x026: 'IDP4', 0x028: 'DMUL', 0x029: 'DADD', 0x02a: 'DSETP',
    0x02b: 'DFMA', 0x030: 'HADD2', 0x031: 'HFMA2', 0x032: 'HMUL2', 0x033: 'HSET2', 0x034: 'HSETP2',
    0x03e: 'F2FP', 0x040: 'HMNMX2', 0x100: 'FLO', 0x101: 'BREV', 0x106: 'I2F', 0x108: 'MUFU',
    0x109: 'POPC', 0x10a: 'F2F', 0x10b: 'F2I', 0x112: 'I2F.64', 0x124: 'OUT', 0x148: 'WARPSYNC',
    0x15d: 'NANOSLEEP', 0x104: 'F2F', 0x045: 'I2FP', 0x082: 'UMOV', 0x105: 'F2I', 0x107: 'FRND', 0x008: 'FSEL', 0x004: 'R2P', 0x003: 'P2R',
    # uniform datapath (UGPR destination)
    0x050: 'UFMNMX', 0x052: 'UFSET', 0x053: 'UFSETP', 0x054: 'UFADD', 0x055: 'UFFMA', 0x056: 'UFMUL',
    0x05a: 'UI2F', 0x05b: 'UF2F', 0x05c: 'UF2I', 0x05d: 'UFRND', 0x04d: 'UIABS', 0x085: 'UIMNMX',
    0x087: 'USEL', 0x08c: 'UISETP', 0x090: 'UIADD3', 0x091: 'ULEA', 0x092: 'ULOP3', 0x096: 'UPRMT',
    0x099: 'USHF', 0x09a: 'USGXT', 0x09b: 'UBMSK', 0x0a4: 'UIMAD', 0x0a5: 'UIMAD.WIDE', 0x0ba: 'UF2FP',
    0x0bd: 'UFLO', 0x0be: 'UBREV', 0x0bf: 'UPOPC',
}

OTHER = {  # 12-bit opcode -> mnemonic
    0x822: 'FSWZADD', 0xc82: 'UMOV', 0x389: 'SHFL', 0x589: 'SHFL', 0x989: 'SHFL', 0xf89: 'SHFL',
    0x81c: 'PLOP3', 0x89c: 'UPLOP3', 0x2ca: 'R2UR', 0x3c2: 'R2UR', 0x3c4: 'REDUX',
    0x361: 'TEX', 0xb60: 'TEX', 0xd61: 'TEX', 0x367: 'TLD', 0xb66: 'TLD', 0xd67: 'TLD',
    0x364: 'TLD4', 0xb63: 'TLD4', 0xd64: 'TLD4', 0x36a: 'TMML', 0xb69: 'TMML',
    0x36d: 'TXD', 0xb6c: 'TXD', 0xd6d: 'TXD', 0x370: 'TXQ', 0xb6f: 'TXQ',
    0x998: 'SULD', 0x99a: 'SULD', 0x99c: 'SUST', 0x99e: 'SUST', 0x394: 'SUATOM', 0x396: 'SUATOM',
    0x3a0: 'SUATOM', 0x381: 'LDG', 0x981: 'LDG', 0x983: 'LDS', 0x984: 'LDL', 0x582: 'LDC', 0xb82: 'LDC',
    0x7ac: 'ULDC', 0xab9: 'ULDC', 0xbac: 'ULDC', 0x8b8: 'LDCG', 0x9ac: 'LDCG', 0x386: 'STG', 0x986: 'STG',
    0x387: 'STL', 0x987: 'STL', 0x388: 'STS', 0x988: 'STS', 0x38c: 'ATOMS', 0x38d: 'ATOM', 0x3a8: 'ATOMG',
    0x3a9: 'ATOMG', 0x98c: 'ATOMS', 0x98e: 'RED', 0x9a3: 'ATOMG', 0x9a6: 'RED', 0x9a8: 'ATOMG',
    0x920: 'AL2P', 0x321: 'ALD', 0x322: 'AST', 0x326: 'IPA', 0x3ad: 'LDTRAM', 0x98f: 'CCTL',
    0x992: 'MEMBAR', 0x355: 'BMOV', 0x356: 'BMOV', 0x942: 'BREAK', 0x945: 'BSSY', 0x941: 'BSYNC',
    0x547: 'BRA', 0x947: 'BRA', 0x94d: 'EXIT', 0x31d: 'BAR', 0xb1d: 'BAR', 0x805: 'CS2R', 0x923: 'ISBERD',
    0x927: 'ISBEWR', 0x95b: 'KILL', 0x949: 'BRX', 0x944: 'CALL', 0x950: 'RET', 0x526: 'IPA', 0xabb: 'LDCU', 0x91a: 'DEPBAR', 0x918: 'NOP', 0x925: 'PIXLD', 0x919: 'S2R', 0x9c3: 'S2UR',
    0x806: 'VOTE', 0x886: 'VOTEU', 0x3a1: 'MATCH', 0x237: 'IMMA', 0x23c: 'HMMA', 0x83b: 'LDSM', 0x23a: 'MOVM',
}


def bits(v, lo, hi):
    """Field [lo, hi) of the 128-bit instruction v."""
    return (v >> lo) & ((1 << (hi - lo)) - 1)


def opcode(v):
    """(mnemonic, form) — form is None for non-ALU instructions."""
    op12 = v & 0xFFF
    if op12 in OTHER:
        return OTHER[op12], None
    op9, form = v & 0x1FF, (v >> 9) & 7
    if op9 in ALU and 1 <= form <= 7:
        return ALU[op9], form
    return 'UNK_%03x' % op12, None


def program_words(code, start=0x80):
    """Yields (offset, 128-bit value) until the program's final EXIT + self-branch."""
    seen_exit = False
    off = start
    while off + 16 <= len(code):
        v = int.from_bytes(code[off:off + 16], 'little')
        name, _ = opcode(v)
        yield off, v
        if name == 'EXIT':
            seen_exit = True
        elif name == 'BRA' and seen_exit:
            return
        off += 16
