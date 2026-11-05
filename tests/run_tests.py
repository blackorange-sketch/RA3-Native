#!/usr/bin/env python3
"""Differential test of the lifter against the real CPU.

Each snippet is assembled with nasm, run natively as a 32-bit program, lifted to C, run in the
64-bit harness, and the register/memory/flag results are compared for many inputs."""
import itertools, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ra3-tests"
CAP = os.environ.get("CAPSTONE", "")          # capstone source dir (with b/libcapstone.a)
os.makedirs(OUT, exist_ok=True)

tests = []   # (name, body, mask_low, mask_of)
def T(name, body, low=0xd5, of=1):
    tests.append((name, body, low, of))

MEM = "mov esi, 0x2000100\n"
ARITH = ["add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test"]
for op in ARITH:
    low = 0xc5 if op in ("and", "or", "xor", "test") else 0xd5   # AF is undefined for logic ops
    pre = "cmp ecx, edx\n" if op in ("adc", "sbb") else ""
    for form in ["eax, ebx", "al, bl", "ax, bx", "ah, cl", "cl, dh", "esi, edi", "ecx, 0x12345678", "cl, 0x80", "cx, 0x8000",
                 "edx, -1", "ebx, 7", "al, 0x7f", "ax, 0xff"]:
        T(f"{op} {form}", f"{pre}{op} {form}", low)
    for form in ["[esi+8], eax", "eax, [esi+8]", "[esi+ecx*0+4], bl" if False else "[esi+4], bl", "dx, [esi+2]", "dword [esi+4], 0x7fffffff", "byte [esi+3], 0x81"]:
        T(f"{op} {form}", f"{pre}{MEM}{op} {form}", low)

for op in ["inc", "dec", "neg", "not"]:
    low = 0xd5
    for r in ["eax", "al", "ax", "ch", "edi"]:
        T(f"{op} {r}", f"cmp ecx, edx\n{op} {r}", low)
    T(f"{op} dword [mem]", f"{MEM}{op} dword [esi+4]", low)
    T(f"{op} byte [mem]", f"{MEM}{op} byte [esi+5]", low)

for op in ["shl", "shr", "sar"]:
    for cnt in ["1", "3", "7", "31", "8", "16"]:
        for r in ["eax", "bl", "cx"]:
            T(f"{op} {r}, {cnt}", f"{op} {r}, {cnt}", 0xc5, 1 if cnt == "1" else 0)
    for r in ["eax", "bl", "cx"]:
        T(f"{op} {r}, cl", f"mov ch, 0\ncmp edx, ebx\n{op} {r}, cl" if r != "cx" else f"cmp edx, ebx\n{op} {r}, cl", 0xc5, 0)
    T(f"{op} [mem], 2", f"{MEM}{op} dword [esi+4], 2", 0xc5, 0)
for op in ["rol", "ror"]:
    for cnt in ["1", "3", "7", "31"]:
        for r in ["eax", "bl", "cx"]:
            T(f"{op} {r}, {cnt}", f"cmp ecx, edx\n{op} {r}, {cnt}", 0x01, 1 if cnt == "1" else 0)

T("mov forms", "mov eax, ebx\nmov cl, dh\nmov ax, di\nmov dword [0x2000200], ecx\nmov byte [0x2000204], ah\nmov esi, [0x2000200]\nmov edx, 0x1234")
T("movzx", "movzx eax, bl\nmovzx edx, cx\nmovzx esi, byte [0x2000300]\nmovzx ax, bh\nmovzx edi, word [0x2000302]")
T("movsx", "movsx eax, bl\nmovsx edx, cx\nmovsx esi, byte [0x2000300]\nmovsx ax, bh\nmovsx edi, word [0x2000302]")
T("lea", "lea eax, [ebx+ecx*4+0x10]\nlea edx, [esi+edi]\nlea ecx, [ecx*8+5]\nlea si, [ebx+2]\nlea edi, [0x1234]", 0xd5)
T("xchg", "xchg eax, ebx\nxchg cl, dl\nxchg [0x2000400], esi\nxchg dx, di")
T("push pop", "push eax\npush ebx\npush 0x7fffffff\npush -3\npop ecx\npop edx\npop esi\npush dword [0x2000100]\npop edi\npop ebx")
T("leave", "push ebp\nmov ebp, esp\nsub esp, 16\nmov eax, 99\nleave")
T("bswap", "bswap eax\nbswap edx")
T("cdq cwde cbw cwd", "cdq\ncwde\ncbw\ncwd")
T("cdq signs", "mov eax, ebx\ncdq")
T("get pc", "call .n\n.n: pop eax")
for form in ["ebx", "cl", "cx", "dword [0x2000100]", "byte [0x2000100]"]:
    T(f"mul {form}", f"mul {form}", 0x01, 1)
    T(f"imul {form}", f"imul {form}", 0x01, 1)
T("imul 2op", "imul eax, ebx\nimul ecx, [0x2000100]", 0x01, 1)
T("imul 3op", "imul eax, ebx, 12345\nimul ecx, edx, -7", 0x01, 1)
T("div", "or ebx, 1\nxor edx, edx\ndiv ebx", 0x00, 0)
T("div small", "or ebx, 0x10000\nand edx, 0xff\ndiv ebx", 0x00, 0)
T("idiv", "and ebx, 0x7fff\nor ebx, 0x100\nmov edx, eax\nsar edx, 31\nidiv ebx\nneg ebx", 0x00, 0)
T("idiv neg", "and ebx, 0x7fff\nor ebx, 0x100\nneg ebx\nmov edx, eax\nsar edx, 31\nidiv ebx", 0x00, 0)
T("div 8", "or bl, 1\nand eax, 0xff\ndiv bl", 0x00, 0)
T("div 16", "or bx, 0x100\nand edx, 0\ndiv bx", 0x00, 0)
for cc in ["e", "ne", "b", "ae", "be", "a", "s", "ns", "p", "np", "o", "no", "l", "ge", "le", "g"]:
    T(f"set{cc}", f"cmp ebx, edx\nset{cc} al\nset{cc} ch")
    T(f"cmov{cc}", f"cmp ebx, edx\ncmov{cc} eax, ecx\ncmov{cc} esi, [0x2000100]")
    T(f"j{cc}", f"cmp ebx, edx\nj{cc} .t\nmov eax, 1\njmp .e\n.t: mov eax, 2\n.e:")
T("loop jcc", "mov ecx, 5\nxor eax, eax\n.l: add eax, ecx\ndec ecx\njnz .l")
T("bsf bsr", "bsf eax, ebx\nbsr edx, ecx\nbsf esi, [0x2000100]", 0x40, 0)
T("bt", "bt eax, 5\nbt ebx, ecx", 0x01, 0)
T("bts btr btc", "bts eax, 3\nbtr ebx, 4\nbtc ecx, edx", 0x01, 0)
T("stc clc cmc", "stc\nadc eax, 0\nclc\nadc ebx, 0\ncmc\nadc ecx, 0")
T("sahf lahf", "cmp eax, ebx\nlahf\nmov bh, ah\nsahf", 0xd5, 0)
T("rep movsd", "mov esi, 0x2000100\nmov edi, 0x2000600\nand ecx, 15\ncld\nrep movsd")
T("rep movsb", "mov esi, 0x2000100\nmov edi, 0x2000700\nand ecx, 31\ncld\nrep movsb")
T("rep stosd", "mov edi, 0x2000800\nand ecx, 15\ncld\nrep stosd")
T("rep stosb", "mov edi, 0x2000900\nand ecx, 31\ncld\nrep stosb")
T("std movsb", "mov esi, 0x2000200\nmov edi, 0x2000a00\nand ecx, 15\nstd\nrep movsb\ncld")
T("lodsd stosw", "mov esi, 0x2000100\nmov edi, 0x2000b00\nlodsd\nlodsb\nstosw\nstosb")
T("repne scasb", "mov edi, 0x2000100\nmov al, [0x2000103]\nmov ecx, 100\ncld\nrepne scasb")
T("repe cmpsb", "mov esi, 0x2000100\nmov edi, 0x2000100\nmov byte [edi+7], 5\nmov byte [esi+7], 6\nmov ecx, 50\ncld\nrepe cmpsb")
T("scasd cmpsd", "mov edi, 0x2000100\nmov esi, 0x2000200\ncld\nscasd\ncmpsd")

SUFFIX = """
push eax
seto al
lahf
and al, {of}
and ah, {low}
mov [0x2000ff1], al
mov [0x2000ff0], ah
pop eax
ret
"""
with open(f"{OUT}/snips.asm", "w") as f:
    f.write("bits 32\norg 0x1000000\n")
    for i, (name, body, low, of) in enumerate(tests):
        f.write(f"; {i}: {name}\nalign 256, db 0xcc\nt{i}:\n{body}\n{SUFFIX.format(low=hex(low), of=hex(of))}\n")
    f.write("align 256, db 0xcc\n")
def run(*a, **k):
    r = subprocess.run(a, capture_output=True, text=True, **k)
    if r.returncode:
        print(r.stdout, r.stderr); sys.exit(1)
    return r
run("nasm", "-f", "bin", f"{OUT}/snips.asm", "-o", f"{OUT}/snips.bin")
data = open(f"{OUT}/snips.bin", "rb").read()
# bin is padded to 256 alignment: keep only complete tests
data = data[: len(tests) * 256]
open(f"{OUT}/snips.bin", "wb").write(data)
# every snippet must fit in 256 bytes
sizes = []
src = open(f"{OUT}/snips.asm").read()

asm = open(f"{HERE}/native_runner.asm").read().replace("%NT%", str(len(tests)))
open(f"{OUT}/native.asm", "w").write(asm)
run("nasm", "-f", "elf32", "-I", OUT + "/", f"{OUT}/native.asm", "-o", f"{OUT}/native.o")
run("ld", "-m", "elf_i386", "-Ttext=0x1000000", "-o", f"{OUT}/native", f"{OUT}/native.o")
nat = subprocess.run([f"{OUT}/native"], capture_output=True)
open(f"{OUT}/native.out", "wb").write(nat.stdout)
if nat.returncode: print("native runner exit", nat.returncode, "after", len(nat.stdout)//44//48, "tests")

inc = f"{CAP}/include"
lib = f"{CAP}/b/libcapstone.a"
run("g++", "-O1", "-std=c++17", "-I", inc, f"{HERE}/lift_tests_main.cpp", f"{HERE}/../tools/lift.cpp", lib, "-o", f"{OUT}/lift_tests")
lr = subprocess.run([f"{OUT}/lift_tests", f"{OUT}/snips.bin", f"{OUT}/tests_gen.inc"], capture_output=True, text=True)
print(lr.stderr, end="")
run("g++", "-O1", "-std=c++17", "-w", "-I", OUT, f"{HERE}/harness.cpp", f"{HERE}/../runtime/guest.cpp", "-o", f"{OUT}/harness")
lif = subprocess.run([f"{OUT}/harness"], capture_output=True)
if lif.stderr: print(lif.stderr.decode(), end="")
a, b = nat.stdout, lif.stdout
print(f"tests {len(tests)}  native bytes {len(a)}  lifted bytes {len(b)}")
RS = 44
bad = {}
for off in range(0, min(len(a), len(b)), RS):
    if a[off:off+RS] != b[off:off+RS]:
        t, it = struct.unpack_from("<II", a, off)
        if t not in bad:
            bad[t] = (it, struct.unpack_from("<11I", a, off), struct.unpack_from("<11I", b, off))
names = "eax ecx edx ebx esp ebp esi edi".split()
for t, (it, x, y) in sorted(bad.items()):
    d = [f"{n}: nat {x[2+i]:08x} lift {y[2+i]:08x}" for i, n in enumerate(names) if x[2+i] != y[2+i]]
    if x[10] != y[10]: d.append("mem/flags differ")
    print(f"FAIL {t} {tests[t][0]!r} iter {it}: " + "; ".join(d))
print(f"passed {len(tests) - len(bad)}/{len(tests)}")
if len(a) != len(b): print("RECORD COUNT MISMATCH (a runner crashed)")
sys.exit(1 if bad or len(a) != len(b) else 0)
