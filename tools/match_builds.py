#!/usr/bin/env python3
"""Match the functions of two builds of the same game executable (ra3_1.12.game and the Steam ra3_1.13.game).

The Steam build has no relocation table and its code is shifted, so functions are compared as disassembly with
every address-like number (globals, call and jump targets) replaced by '#'. Functions whose first instructions are
unique in both builds are matched; the numbers at the same places in matched code then translate addresses: called
functions, globals, return addresses. Requires the capstone Python module.

usage: match_builds.py old.game old.functions.txt new.game new.functions.txt [addr ...]
Prints statistics and "old -> new" per requested address. Only addresses and counts are printed - no game code.
"""
import re
import struct
import sys
from collections import Counter, defaultdict

import capstone

KEY_INSNS = 20
NUM = re.compile(r'0x[0-9a-f]+')


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
        self.size = struct.unpack_from('<I', d, opt + 56)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vsz, rptr, rsz))
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def read(self, va, n):
        rva = va - self.base
        for _, sva, vsz, rptr, rsz in self.secs:
            if sva <= rva < sva + rsz:
                o = rptr + rva - sva
                return self.d[o:o + min(n, sva + rsz - rva)]
        return b''

    def isaddr(self, v):
        return self.base <= v < self.base + self.size

    def insns(self, va, maxn, limit):
        out = []
        for i in self.cs.disasm(self.read(va, limit), va):
            nums = [int(x, 16) for x in NUM.findall(i.op_str)]
            norm = NUM.sub(lambda m: '#' if self.isaddr(int(m.group(0), 16)) else m.group(0), i.op_str)
            out.append((i.address, i.mnemonic + ' ' + norm, [n for n in nums if self.isaddr(n)]))
            if len(out) >= maxn or i.mnemonic in ('ret', 'jmp') and len(out) > 2:
                break
        return out


def load_funcs(path):
    return sorted(int(l.split()[0], 16) for l in open(path) if l.strip())


def main():
    old, oldf, new, newf = sys.argv[1:5]
    want = [int(a, 16) for a in sys.argv[5:]]
    A, B = PE(old), PE(new)
    fa, fb = load_funcs(oldf), load_funcs(newf)
    print(f'old: entry {A.entry:#x} {len(fa)} functions; new: entry {B.entry:#x} {len(fb)} functions')

    def keyed(pe, fs):
        m, ins = defaultdict(list), {}
        for k, f in enumerate(fs):
            lim = (fs[k + 1] - f) if k + 1 < len(fs) else 4096
            seq = pe.insns(f, 4000, min(lim, 65536))
            ins[f] = seq
            if len(seq) >= 3:
                m[tuple(s[1] for s in seq[:KEY_INSNS])].append(f)
        return m, ins
    ka, ia = keyed(A, fa)
    kb, ib = keyed(B, fb)
    match = {}
    for k, v in ka.items():
        if len(v) == 1 and len(kb.get(k, [])) == 1:
            match[v[0]] = kb[k][0]
    print(f'matched {len(match)} of {len(fa)} functions by their first {KEY_INSNS} instructions')

    # numbers at the same places of matched functions (whole bodies while the instructions agree)
    vote = defaultdict(Counter)
    code = {}   # old instruction address -> new instruction address
    for a, b in match.items():
        for (xa, ta, na), (xb, tb, nb) in zip(ia[a], ib[b]):
            if ta != tb:
                break
            code[xa] = xb
            for p, q in zip(na, nb):
                vote[p][q] += 1
    deltas = Counter(b - a for a, b in match.items())
    print('most common shifts: ' + ', '.join(f'{d:+#x} x{c}' for d, c in deltas.most_common(6)))
    print(f'{len(vote)} addresses translated through matched code')

    for x in want:
        if x in match:
            print(f'{x:08x} -> {match[x]:08x} (function)')
        elif x in vote:
            y, n = vote[x].most_common(1)[0]
            print(f'{x:08x} -> {y:08x} ({n} references)')
        elif x in code:
            print(f'{x:08x} -> {code[x]:08x} (instruction)')
        else:
            # nearest translated neighbour (struct fields, arrays, return addresses after a call)
            best = None
            for d in range(1, 0x200):
                for y in (x - d, x + d):
                    if y in vote:
                        best = (y, vote[y].most_common(1)[0][0] - y)
                        break
                    if y in code:
                        best = (y, code[y] - y)
                        break
                if best:
                    break
            print(f'{x:08x} -> ' + (f'{x + best[1]:08x} (near {best[0]:08x})' if best else '?'))


if __name__ == '__main__':
    main()
