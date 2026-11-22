#!/usr/bin/env python3
"""Match the functions of two builds of the same game executable (e.g. ra3_1.12.game and the Steam ra3_1.13.game).

Each function is keyed by its first bytes with every relocated dword zeroed (absolute addresses differ between
builds, the code itself does not). Functions whose key is unique in both builds are matched; addresses the runtime
uses (functions, return addresses, globals) are then translated through the matched functions.

usage: match_builds.py old.game old.functions.txt new.game new.functions.txt [addr ...]
Prints match statistics and one line per requested address: "old -> new" (or "old -> ?").
Only addresses and counts are printed - no game code.
"""
import struct
import sys
from collections import Counter, defaultdict

KEY_BYTES = 48


class PE:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.d = d
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.entry = self.base + struct.unpack_from('<I', d, opt + 16)[0]
        reloc_rva, reloc_sz = struct.unpack_from('<II', d, opt + 96 + 5 * 8)
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vsz, rptr, rsz))
        self.relocs = set()
        p = self.rva2off(reloc_rva) if reloc_rva else None
        end = p + reloc_sz if p is not None else 0
        p = p or 0
        while reloc_rva and p < end:
            page, size = struct.unpack_from('<II', d, p)
            if size < 8:
                break
            for k in range((size - 8) // 2):
                e = struct.unpack_from('<H', d, p + 8 + 2 * k)[0]
                if e >> 12 == 3:
                    self.relocs.add(self.base + page + (e & 0xfff))
            p += size

    def rva2off(self, rva):
        for _, va, vsz, rptr, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return rptr + (rva - va) if rva - va < rsz else None
        return None

    def read(self, va, n):
        o = self.rva2off(va - self.base)
        return self.d[o:o + n] if o is not None else b''

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    def key(self, va, n):
        b = bytearray(self.read(va, n))
        for i in range(-3, len(b)):
            if va + i in self.relocs:
                for j in range(max(0, i), min(len(b), i + 4)):
                    b[j] = 0
        return bytes(b)


def load_funcs(path):
    return sorted(int(l.split()[0], 16) for l in open(path) if l.strip())


def main():
    old, oldf, new, newf = sys.argv[1:5]
    want = [int(a, 16) for a in sys.argv[5:]]
    A, B = PE(old), PE(new)
    fa, fb = load_funcs(oldf), load_funcs(newf)
    print(f'old: base {A.base:#x} entry {A.entry:#x} {len(fa)} functions {len(A.relocs)} relocations')
    print(f'new: base {B.base:#x} entry {B.entry:#x} {len(fb)} functions {len(B.relocs)} relocations')
    for (n1, va1, vs1, _, _), (n2, va2, vs2, _, _) in zip(A.secs, B.secs):
        print(f'  section {n1:8} {A.base + va1:#010x} {vs1:#x}   {n2:8} {B.base + va2:#010x} {vs2:#x}')

    def sizes(fs):
        return {f: (fs[i + 1] - f if i + 1 < len(fs) else 64) for i, f in enumerate(fs)}
    sa, sb = sizes(fa), sizes(fb)

    def keyed(pe, fs, sz):
        m = defaultdict(list)
        for f in fs:
            n = min(KEY_BYTES, sz[f])
            if n >= 8:
                m[(sz[f], pe.key(f, n))].append(f)
        return m
    ka, kb = keyed(A, fa, sa), keyed(B, fb, sb)
    match = {}
    for k, v in ka.items():
        if len(v) == 1 and len(kb.get(k, [])) == 1:
            match[v[0]] = kb[k][0]
    deltas = Counter(b - a for a, b in match.items())
    print(f'matched {len(match)} of {len(fa)} functions uniquely (same size and code)')
    print('most common shifts: ' + ', '.join(f'{d:+#x} x{c}' for d, c in deltas.most_common(8)))

    # globals: votes from relocated dwords at the same offset in matched functions
    gvote = defaultdict(Counter)
    for a, b in match.items():
        for off in range(sa[a]):
            if a + off in A.relocs:
                va, vb = A.u32(a + off), B.u32(b + off)
                if va is not None and vb is not None:
                    gvote[va][vb - va] += 1

    def translate(x):
        # inside a matched function (start, return address)?
        i = max((f for f in fa if f <= x), default=None)
        if i is not None and i in match and x - i < sa[i]:
            return match[i] + (x - i), 'code'
        # a global referenced by matched code (exact or nearby: struct fields, arrays)
        best = None
        for d in range(0, 0x400, 4):
            for y in (x - d, x + d):
                if y in gvote:
                    shift, n = gvote[y].most_common(1)[0]
                    best = (x + shift, f'data, {n} references near {y:#x}')
                    break
            if best:
                return best
        return None, ''
    for x in want:
        y, how = translate(x)
        print(f'{x:08x} -> ' + (f'{y:08x} ({how})' if y is not None else '?'))


if __name__ == '__main__':
    main()
