#!/usr/bin/env python3
"""Imports of a game build (its whole import table, not only the call sites the analyser saw) that the runtime has no
implementation for - each would stop the game with "unimplemented import" when first called.

usage: missing_imports.py game.exe runtime_dir [known_good.exe]
(imports the known-good build also has are left out: those work, by whatever mechanism)
"""
import glob
import os
import re
import struct
import sys


def imports_of(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    secs = [struct.unpack_from('<IIII', d, opt + optsz + 40 * i + 8) for i in range(nsec)]

    def off(rva):
        for vsz, va, rsz, rptr in secs:
            if va <= rva < va + rsz:
                return rptr + rva - va
        return None
    imports = []
    p = off(struct.unpack_from('<I', d, opt + 96 + 8)[0])
    while p is not None:
        oft, _, _, name_rva, ft = struct.unpack_from('<IIIII', d, p)
        if not name_rva:
            break
        dll = d[off(name_rva):].split(b'\0')[0].decode()
        t, k = off(oft or ft), 0
        while True:
            v = struct.unpack_from('<I', d, t + 4 * k)[0]
            if not v:
                break
            nm = f'#{v & 0xffff}' if v & 0x80000000 else d[off(v) + 2:].split(b'\0')[0].decode()
            imports.append((dll, nm))
            k += 1
        p += 20
    return imports


def main():
    path, rdir = sys.argv[1], sys.argv[2]
    imports = imports_of(path)
    known = set(imports_of(sys.argv[3])) if len(sys.argv) > 3 else set()
    have = set()
    for f in glob.glob(os.path.join(rdir, '*.cpp')):
        s = open(f, encoding='utf-8', errors='replace').read()
        defs = dict(re.findall(r'#define\s+(\w+)\s+"([^"]*)"', s))
        for m in re.finditer(r'IMP_ID\(\s*(?:"[^"]*"|\w+)\s*,\s*((?:"[^"]*"|\w+)(?:\s*(?:"[^"]*"|\w+))*)\s*,', s):
            parts = re.findall(r'"([^"]*)"|(\w+)', m.group(1))
            have.add(''.join(a if a else defs.get(b, b) for a, b in parts))
        for m in re.finditer(r'IMP_(?:STD|C)\(\s*(?:"[^"]*"|\w+)\s*,\s*(\w+)', s):
            have.add(m.group(1))
    miss = [f'{dll}!{nm}' for dll, nm in imports if nm not in have and (dll, nm) not in known]
    print(f'imports {len(imports)}, without an implementation {len(miss)}:')
    for x in miss:
        print('  ' + x)


if __name__ == '__main__':
    main()
