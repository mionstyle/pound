#!/usr/bin/env python3
"""Static analysis of decompressed NSO images (see tools/nso_info.c), to scope a
static recompilation of a Switch / Switch 2 title.

usage: analyze.py <main.bin> [sdk.bin] [rtld.bin] [--json out.json]

Reports, per module: dynamic symbols (imports/exports), relocations, function
starts (from .eh_frame_hdr, exports, BL targets and pointer relocations), SVC
numbers, and which imports of `main` are satisfied by the other modules.
"""
import collections
import json
import re
import struct
import subprocess
import sys

DT = {1: "NEEDED", 2: "PLTRELSZ", 4: "HASH", 5: "STRTAB", 6: "SYMTAB", 7: "RELA", 8: "RELASZ",
      10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI", 23: "JMPREL", 25: "INIT_ARRAY",
      27: "INIT_ARRAYSZ", 0x6FFFFEF5: "GNU_HASH", 0x6FFFFFF9: "RELACOUNT"}

R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027
STT_FUNC, STT_OBJECT = 2, 1


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def read_encoded(d, o, enc, base):
    """Minimal DWARF pointer decoding (what .eh_frame_hdr uses in practice)."""
    fmt = enc & 0x0F
    size, val = {0x03: (4, u32(d, o)), 0x0B: (4, struct.unpack_from("<i", d, o)[0]),
                 0x04: (8, struct.unpack_from("<Q", d, o)[0]),
                 0x0C: (8, struct.unpack_from("<q", d, o)[0])}[fmt]
    app = enc & 0x70
    if app == 0x10:   # pcrel
        val += o
    elif app == 0x30:  # datarel
        val += base
    return val, size


class Module:
    def __init__(self, name, path):
        self.name = name
        self.d = open(path, "rb").read()
        d = self.d
        self.mod0 = u32(d, 4)
        if d[self.mod0:self.mod0 + 4] != b"MOD0":
            raise ValueError(f"{name}: no MOD0 header")
        f = struct.unpack_from("<6i", d, self.mod0 + 4)
        self.dynamic = self.mod0 + f[0]
        self.bss = (self.mod0 + f[1], self.mod0 + f[2])
        self.eh_hdr = (self.mod0 + f[3], self.mod0 + f[4])
        self.parse_dynamic()
        self.parse_symbols()
        self.parse_relocs()

    # -- ELF dynamic info -------------------------------------------------
    def parse_dynamic(self):
        self.dyn = {}
        o = self.dynamic
        while True:
            tag, val = struct.unpack_from("<qQ", self.d, o)
            o += 16
            if tag == 0:
                break
            self.dyn.setdefault(DT.get(tag, hex(tag)), val)

    def cstr(self, off):
        base = self.dyn["STRTAB"] + off
        return self.d[base:self.d.index(b"\0", base)].decode("ascii", "replace")

    def parse_symbols(self):
        symtab, strtab = self.dyn["SYMTAB"], self.dyn["STRTAB"]
        if "HASH" in self.dyn:
            count = u32(self.d, self.dyn["HASH"] + 4)
        else:
            count = (strtab - symtab) // 24 if strtab > symtab else 0
        self.syms = []
        for i in range(count):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.d, symtab + 24 * i)
            self.syms.append({"name": self.cstr(name) if name else "", "type": info & 0xF,
                              "bind": info >> 4, "shndx": shndx, "value": value, "size": size})
        self.imports = [s for s in self.syms if s["shndx"] == 0 and s["name"]]
        self.exports = [s for s in self.syms if s["shndx"] != 0 and s["name"]]

    def parse_relocs(self):
        self.relocs = []
        for start_key, size_key in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ")):
            if start_key not in self.dyn:
                continue
            start, size = self.dyn[start_key], self.dyn.get(size_key, 0)
            for o in range(start, start + size, 24):
                off, info, addend = struct.unpack_from("<QQq", self.d, o)
                self.relocs.append((off, info & 0xFFFFFFFF, info >> 32, addend))

        # DT_RELR (0x24) / DT_RELRSZ (0x23): packed RELATIVE relocations with implicit addends
        # (the addend is the 64-bit value already stored at the target).
        relr, relrsz = self.dyn.get("0x24"), self.dyn.get("0x23", 0)
        if relr is None:
            return
        where = 0
        for o in range(relr, relr + relrsz, 8):
            entry = struct.unpack_from("<Q", self.d, o)[0]
            if entry & 1 == 0:
                targets = [entry]
                where = entry + 8
            else:
                targets = [where + 8 * i for i in range(63) if (entry >> (i + 1)) & 1]
                where += 63 * 8
            for t in targets:
                if t + 8 <= len(self.d):
                    self.relocs.append((t, R_RELATIVE, 0, struct.unpack_from("<Q", self.d, t)[0]))

    # -- code ------------------------------------------------------------
    @property
    def text(self):
        # .text is the first segment; it ends where MOD0 / .rodata begin.
        return (0, self.text_end)

    def find_text_end(self, text_size):
        self.text_end = text_size

    def eh_frame_functions(self):
        d, (start, end) = self.d, self.eh_hdr
        if end <= start:
            return set()
        version, ptr_enc, cnt_enc, tbl_enc = d[start:start + 4]
        o = start + 4
        _, n = read_encoded(d, o, ptr_enc, start)
        o += n
        count, n = read_encoded(d, o, cnt_enc, start)
        o += n
        funcs = set()
        for _ in range(count):
            loc, n = read_encoded(d, o, tbl_enc, start)
            o += 2 * n
            funcs.add(loc)
        return funcs

    def scan_code(self):
        lo, hi = self.text
        words = struct.unpack_from(f"<{(hi - lo) // 4}I", self.d, lo)
        bl_targets, svcs = set(), collections.Counter()
        pac = collections.Counter()
        pac_ops = {0xD503233F: "paciasp", 0xD50323BF: "autiasp", 0xD503237F: "pacibsp",
                   0xD50323FF: "autibsp", 0xD65F0BFF: "retaa", 0xD65F0FFF: "retab"}
        for i, w in enumerate(words):
            if (w & 0xFC000000) == 0x94000000:            # BL imm26
                imm = w & 0x03FFFFFF
                if imm & 0x02000000:
                    imm -= 1 << 26
                t = lo + 4 * i + 4 * imm
                if lo <= t < hi:
                    bl_targets.add(t)
            elif (w & 0xFFE0001F) == 0xD4000001:          # SVC #imm16
                svcs[(w >> 5) & 0xFFFF] += 1
            elif w in pac_ops:
                pac[pac_ops[w]] += 1
            elif (w & 0xFEFFF800) == 0xD63F0800:          # BLRAA/BLRAB/BRAA/BRAB family
                pac["blra*/bra*"] += 1
        return bl_targets, svcs, pac


def demangle(names):
    if not names:
        return []
    out = subprocess.run(["llvm-cxxfilt"], input="\n".join(names), capture_output=True, text=True).stdout
    return out.splitlines()


def namespace_of(pretty):
    m = re.match(r"(?:(?:const|unsigned|void|int|bool|char|long)\s+)*([A-Za-z_]\w*(?:::[A-Za-z_]\w*)?)", pretty)
    if not m:
        return "?"
    ns = m.group(1)
    if ns.startswith("nn::"):
        return ns
    if ns.startswith(("nvn", "NVN")):
        return "nvn"
    if ns in ("std", "__cxa", "__cxxabiv1") or ns.startswith(("__cxa", "_Unwind", "__")):
        return "C/C++ runtime"
    if "::" not in ns and re.fullmatch(r"[a-z_][a-z0-9_]*", ns):
        return "libc"
    return ns.split("::")[0]


def text_size_from_nso(path):
    """Read .text size from the original (compressed) NSO header if given, else guess."""
    return None


def main(argv):
    out_json = None
    if "--json" in argv:
        i = argv.index("--json")
        out_json = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]

    names = ["main", "sdk", "rtld"]
    mods = []
    for name, path in zip(names, argv[1:]):
        m = Module(name, path)
        # .text ends at the MOD0 header (it sits at the start of .rodata, page aligned).
        m.find_text_end(m.mod0 & ~0xFFF)
        mods.append(m)

    report = {}
    exported_by = {}
    for m in mods[1:]:
        for s in m.exports:
            exported_by.setdefault(s["name"], m.name)

    for m in mods:
        bl, svcs, pac = m.scan_code()
        eh = m.eh_frame_functions()
        lo, hi = m.text
        exp_funcs = {s["value"] for s in m.exports if s["type"] == STT_FUNC}
        rel_types = collections.Counter(r[1] for r in m.relocs)
        ptr_targets = {r[3] for r in m.relocs if r[1] == R_RELATIVE and lo <= r[3] < hi}
        all_funcs = eh | exp_funcs | bl | ptr_targets

        print(f"\n===== {m.name}  (.text 0x{hi:x} bytes = {hi // 4:,} instructions)")
        print(f"dynamic symbols : {len(m.syms):,}  imports {len(m.imports):,}  exports {len(m.exports):,}")
        print("relocations     : " + ", ".join(f"{ {R_RELATIVE: 'RELATIVE', R_GLOB_DAT: 'GLOB_DAT', R_JUMP_SLOT: 'JUMP_SLOT', R_ABS64: 'ABS64'}.get(t, t)} {c:,}" for t, c in rel_types.most_common()))
        print(f"function starts : eh_frame {len(eh):,} | exported {len(exp_funcs):,} | BL targets {len(bl):,} | "
              f"code pointers {len(ptr_targets):,} | union {len(all_funcs):,}")
        print(f"  not in eh_frame: BL targets {len(bl - eh):,}, code pointers {len(ptr_targets - eh):,}")
        print("PAC instructions: " + (", ".join(f"{k} {v:,}" for k, v in pac.most_common()) or "none"))
        if svcs:
            print("SVC numbers     : " + " ".join(f"0x{n:02x}" for n in sorted(svcs)) + f"  ({len(svcs)} distinct)")

        rep = {"text_bytes": hi, "imports": len(m.imports), "exports": len(m.exports),
               "functions": {"eh_frame": len(eh), "exports": len(exp_funcs), "bl": len(bl),
                             "code_pointers": len(ptr_targets), "union": len(all_funcs)},
               "pac": dict(pac), "svcs": sorted(svcs)}

        if m.name == "main":
            pretty = demangle([s["name"] for s in m.imports])
            groups = collections.Counter()
            unresolved = []
            by_module = collections.Counter()
            for s, p in zip(m.imports, pretty):
                groups[namespace_of(p)] += 1
                src = exported_by.get(s["name"])
                by_module[src or "UNRESOLVED"] += 1
                if not src:
                    unresolved.append(p)
            print("imports resolved: " + ", ".join(f"{k} {v:,}" for k, v in by_module.most_common()))
            print("imports by area (top 30):")
            for ns, c in groups.most_common(30):
                print(f"  {c:6,}  {ns}")
            if unresolved:
                print(f"unresolved imports ({len(unresolved)}), first 15:")
                for p in unresolved[:15]:
                    print("   ", p[:110])
            rep["import_areas"] = dict(groups)
            rep["unresolved"] = unresolved
        report[m.name] = rep

    if out_json:
        with open(out_json, "w") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main(sys.argv)
