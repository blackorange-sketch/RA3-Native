#!/usr/bin/env python3
"""Where does a game build use a string? Prints, for every function referencing it, the calls (with import names)
and the compares/tests the function makes, in order - enough to see what a check depends on, without the code.

usage: xref.py game.exe game.functions.txt "text" [...]
"""
import bisect
import re
import struct
import sys

import capstone


def main():
    path, funcs_path, texts = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<I', d, opt + 28)[0]
    imp_rva = struct.unpack_from('<I', d, opt + 96 + 8)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, vsz, rptr, rsz))

    def off(rva):
        for va, vsz, rptr, rsz in secs:
            if va <= rva < va + rsz:
                return rptr + rva - va
        return None

    def va_of(fo):
        for va, vsz, rptr, rsz in secs:
            if rptr <= fo < rptr + rsz:
                return base + va + fo - rptr
        return None

    # IAT slot -> "dll!name"
    iat = {}
    p = off(imp_rva)
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
            iat[base + ft + 4 * k] = f'{dll}!{nm}'
            k += 1
        p += 20
    funcs = sorted(int(l.split()[0], 16) for l in open(funcs_path) if l.strip())
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for text in texts:
        for enc in (text.encode('latin1'), text.encode('utf-16-le')):
            at = d.find(enc)
            while at >= 0:
                sva = va_of(at)
                refs = []
                needle = struct.pack('<I', sva)
                r = d.find(needle)
                while r >= 0:
                    rv = va_of(r)
                    if rv:
                        refs.append(rv)
                    r = d.find(needle, r + 1)
                print(f'"{text}" at {sva:08x}, referenced from ' + ', '.join(f'{x:08x}' for x in refs))
                for rv in refs[:4]:
                    i = bisect.bisect_right(funcs, rv) - 1
                    if i < 0:
                        continue
                    f, end = funcs[i], funcs[i + 1] if i + 1 < len(funcs) else funcs[i] + 4096
                    print(f'  function {f:08x} (reference at {rv:08x}):')
                    code = d[off(f - base):off(f - base) + min(end - f, 8192)]
                    for ins in cs.disasm(code, f):
                        m = ins.mnemonic
                        if m == 'call' or m.startswith('cmp') or m == 'test' or ins.address == rv - 1 or (m.startswith('j') and m != 'jmp'):
                            note = ''
                            if m == 'call' and 'dword ptr [0x' in ins.op_str:
                                slot = int(ins.op_str.split('[0x')[1].rstrip(']'), 16)
                                note = '  ; ' + iat.get(slot, '?')
                            mark = '  <-- string' if ins.address <= rv < ins.address + ins.size else ''
                            print(f'    {ins.address:08x} {m} {ins.op_str}{note}{mark}')
                at = d.find(enc, at + 1)


def strings_and_calls(path, funcs_path, ranges):
    """For address ranges (a-b) or functions (a): the strings pushed/loaded and the calls made, in order."""
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<I', d, opt + 28)[0]
    secs = [struct.unpack_from('<IIII', d, opt + optsz + 40 * i + 8) for i in range(nsec)]

    def off(rva):
        for vsz, va, rsz, rptr in secs:
            if va <= rva < va + rsz:
                return rptr + rva - va
        return None
    funcs = sorted(int(l.split()[0], 16) for l in open(funcs_path) if l.strip())
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for r in ranges:
        if '-' in r:
            a, b = (int(x, 16) for x in r.split('-'))
        else:
            a = int(r, 16)
            i = bisect.bisect_right(funcs, a)
            b = funcs[i] if i < len(funcs) else a + 4096
        print(f'  {a:08x}-{b:08x}:')
        o = off(a - base)
        for ins in cs.disasm(d[o:o + (b - a)], a):
            txt = ''
            for tok in re.findall(r'0x[0-9a-f]+', ins.op_str):
                if True:
                    v = int(tok, 16)
                    so = off(v - base) if base <= v < base + 0x2000000 else None
                    if so is not None:
                        raw = d[so:so + 80].split(b'\0')[0]
                        if len(raw) >= 4 and all(32 <= ch < 127 for ch in raw):
                            txt = ' "' + raw.decode() + '"'
            if ins.mnemonic in ('call', 'ret') or txt or ins.mnemonic.startswith('cmp') or ins.mnemonic in ('test', 'setne', 'sete', 'mov') and ('bl' in ins.op_str.split(',')[0]):
                print(f'    {ins.address:08x} {ins.mnemonic} {ins.op_str}{txt}')


if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[3] == '--at':
        strings_and_calls(sys.argv[1], sys.argv[2], sys.argv[4:])
        sys.exit(0)
    main()
