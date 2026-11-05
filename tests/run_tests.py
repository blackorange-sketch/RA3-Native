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



# ---- lock-prefixed / atomic forms (sequential semantics; atomicity itself is not testable here)
for op in ["add", "sub", "and", "or", "xor"]:
    low = 0xc5 if op in ("and", "or", "xor") else 0xd5
    T(f"lock {op} dword", MEM + f"lock {op} dword [esi+4], eax", low)
    T(f"lock {op} byte", MEM + f"lock {op} byte [esi+5], bl", low)
    T(f"lock {op} word imm", MEM + f"lock {op} word [esi+6], 0x1234", low)
T("lock inc dec", MEM + "lock inc dword [esi+4]\nlock dec word [esi+8]\nlock inc byte [esi+12]")
T("lock xadd", MEM + "lock xadd [esi+4], eax\nlock xadd [esi+8], bl\nxadd [esi+12], cx")
T("lock cmpxchg hit", MEM + "mov eax, [esi+4]\nlock cmpxchg [esi+4], ebx")
T("lock cmpxchg miss", MEM + "lock cmpxchg [esi+4], ebx")
T("lock cmpxchg byte word", MEM + "lock cmpxchg [esi+4], bl\nlock cmpxchg [esi+8], cx")
T("xchg mem", MEM + "xchg [esi+4], eax\nxchg bl, [esi+9]\nxchg [esi+10], cx")

# ---- SSE (single precision, random bit patterns incl. NaN/denormals)
LD4 = """mov [0x2000f40], eax
mov [0x2000f44], ebx
mov [0x2000f48], ecx
mov [0x2000f4c], edx
mov [0x2000f50], esi
mov [0x2000f54], edi
mov [0x2000f58], ebx
mov [0x2000f5c], eax
movups xmm0, [0x2000f40]
movups xmm1, [0x2000f50]
"""
ST4 = "\nmovups [0x2000f60], xmm0\nmovups [0x2000f70], xmm1"
for op in ["addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps", "andps", "orps", "xorps", "andnps", "unpcklps", "unpckhps", "movlhps", "movhlps"]:
    T(f"sse {op}", LD4 + f"{op} xmm0, xmm1" + ST4)
    T(f"sse {op} mem", LD4 + f"{op} xmm0, [0x2000f50]" + ST4) if op not in ("movlhps", "movhlps") else None
for op in ["addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss"]:
    T(f"sse {op}", LD4 + f"{op} xmm0, xmm1" + ST4)
    T(f"sse {op} mem", LD4 + f"{op} xmm0, [0x2000f50]" + ST4)
for imm in [0x00, 0x1b, 0xe4, 0x55, 0xaa, 0xc6]:
    T(f"shufps {imm:#x}", LD4 + f"shufps xmm0, xmm1, {imm}" + ST4)
for p in ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]:
    T(f"cmp{p}ps", LD4 + f"cmp{p}ps xmm0, xmm1" + ST4)
    T(f"cmp{p}ss", LD4 + f"cmp{p}ss xmm0, xmm1" + ST4)
T("comiss", LD4 + "comiss xmm0, xmm1")
T("ucomiss", LD4 + "ucomiss xmm1, xmm0")
T("comiss mem", LD4 + "comiss xmm0, [0x2000f50]")
T("movss forms", LD4 + "movss xmm0, xmm1\nmovss xmm2, [0x2000f48]\nmovss [0x2000f80], xmm1\nmovups [0x2000f90], xmm2" + ST4)
T("movaps forms", LD4 + "movaps xmm2, xmm1\nmovaps [0x2000fa0], xmm0\nmovaps xmm3, [0x2000fa0]\nmovups [0x2000f90], xmm3" + ST4)
T("movlps movhps", LD4 + "movlps xmm0, [0x2000f50]\nmovhps xmm1, [0x2000f40]\nmovlps [0x2000f80], xmm1\nmovhps [0x2000f88], xmm0" + ST4)
T("movd movq", "movd xmm0, eax\nmovd xmm1, [0x2000f00]\nmovd ecx, xmm0\nmovd [0x2000f10], xmm1\nmovq xmm2, xmm1\nmovq [0x2000f20], xmm2\nmovq xmm3, [0x2000f20]\nmovups [0x2000f60], xmm0\nmovups [0x2000f70], xmm3")
T("cvtsi2ss", "cvtsi2ss xmm0, eax\ncvtsi2ss xmm1, [0x2000f00]\nmovups [0x2000f60], xmm0\nmovups [0x2000f70], xmm1")
T("cvttss2si", LD4 + "cvttss2si eax, xmm0\ncvttss2si edx, xmm1" + ST4)
T("cvtss2si", LD4 + "cvtss2si eax, xmm0\ncvtss2si edx, xmm1" + ST4)
T("movmskps", LD4 + "movmskps eax, xmm0\nmovmskps edx, xmm1" + ST4)

# ---- x87: 53-bit precision (matches double arithmetic) and 24-bit precision (matches float arithmetic)
def FP(pc):  # sets the control word and loads two integers: st1 = eax, st0 = ebx
    return f"""mov dword [0x2000f00], {pc}
fldcw [0x2000f00]
mov [0x2000f10], eax
mov [0x2000f14], ebx
fild dword [0x2000f10]
fild dword [0x2000f14]
"""
def stores(depth):
    return "".join(f"\nfstp qword [0x{0x2000f20 + 8 * i:x}]" for i in range(depth))
def depth_of(code, start=2):
    d = start
    for line in code.split("\n"):
        m = line.split(" ")[0]
        if m.startswith("fld") or m.startswith("fild"): d += 1
        elif m in ("faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp"): d -= 1
    return d
FPST = stores(2)
for pcn, pc in [("53", "0x27f"), ("24", "0x7f")]:
    pre = FP(pc)
    for op in ["fadd st0, st1", "fadd st1, st0", "faddp st1, st0", "fsub st0, st1", "fsub st1, st0", "fsubp st1, st0", "fsubr st0, st1", "fsubr st1, st0", "fsubrp st1, st0",
               "fmul st0, st1", "fmul st1, st0", "fmulp st1, st0", "fdiv st0, st1", "fdiv st1, st0", "fdivp st1, st0", "fdivr st0, st1", "fdivr st1, st0", "fdivrp st1, st0",
               "fxch st1", "fchs", "fabs", "fsqrt", "fld st1\nfadd st0, st1", "fldz\nfaddp st2, st0", "fld1\nfdivp st1, st0"]:
        T(f"x87/{pcn} {op}", pre + op + stores(depth_of(op)))
    # memory operands: take sane floats from the integer inputs first
    sane = "fst dword [0x2000f18]\nfst qword [0x2000f1c]\n"
    for op in ["fadd dword [0x2000f18]", "fadd qword [0x2000f1c]", "fsub dword [0x2000f18]", "fsubr qword [0x2000f1c]", "fmul dword [0x2000f18]", "fdiv dword [0x2000f18]",
               "fdivr qword [0x2000f1c]"]:
        T(f"x87/{pcn} {op}", pre + sane + op + FPST)
    for op in ["fiadd dword [0x2000f10]", "fisub dword [0x2000f14]", "fimul dword [0x2000f10]", "fidiv dword [0x2000f14]", "fidivr dword [0x2000f10]", "fiadd word [0x2000f10]", "fimul word [0x2000f14]"]:
        T(f"x87/{pcn} {op}", pre + op + FPST)
    T(f"x87/{pcn} fst forms", pre + "fst dword [0x2000f30]\nfst qword [0x2000f38]\nfst st2\nfstp dword [0x2000f40]" + stores(2))
    T(f"x87/{pcn} fld mem", pre + sane + "fld dword [0x2000f18]\nfld qword [0x2000f1c]" + stores(4))
    T(f"x87/{pcn} constants", pre + "fstp st0\nfstp st0\nfld1\nfldz\nfldpi\nfldl2e\nfldln2\nfldl2t\nfldlg2" + stores(7))
for mode, name in [(0x37f, "nearest"), (0x77f, "down"), (0xb7f, "up"), (0xf7f, "trunc")]:
    T(f"x87 fist {name}", FP("0x27f") + f"fdivp st1, st0\nmov dword [0x2000f00], {mode}\nfldcw [0x2000f00]\nfld st0\nfistp dword [0x2000f34]\nfld st0\nfistp word [0x2000f38]\nfistp qword [0x2000f40]\nmov dword [0x2000f00], 0x27f\nfldcw [0x2000f00]")
T("x87 fnstcw fldcw", "mov dword [0x2000f04], 0x27f\nfldcw [0x2000f04]\nfnstcw [0x2000f08]\nmov dword [0x2000f04], 0xb7f\nfldcw [0x2000f04]\nfnstcw [0x2000f0c]\nmov dword [0x2000f04], 0x27f\nfldcw [0x2000f04]")
for op in ["fcom st1", "fcomp st1", "fucom st1", "fucomp st1", "fcom", "fcompp", "fucompp"]:
    pops = 2 if op in ("fcompp", "fucompp") else 1 if op.endswith("p st1") else 0
    T(f"x87 {op} + fnstsw", FP("0x27f") + op + "\nfnstsw ax\nand eax, 0x4500\nmov [0x2000f20], eax" + stores(2 - pops))
for op in ["fcomi st0, st1", "fcomip st0, st1", "fucomi st0, st1", "fucomip st0, st1"]:
    pops = 1 if op.startswith(("fcomip", "fucomip")) else 0
    T(f"x87 {op}", FP("0x27f") + op + stores(2 - pops), 0xc5 | 0x10, 1)
T("x87 fcom mem", FP("0x27f") + "fst qword [0x2000f1c]\nfcomp qword [0x2000f1c]\nfnstsw ax\nand eax, 0x4500" + stores(1))
T("x87 fcom sahf", FP("0x27f") + "fcompp\nfnstsw ax\nand eax, 0x4500\nsahf", 0x45, 0)
T("x87 misc", FP("0x27f") + "fxch\nfincstp\nfdecstp" + stores(2))

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
