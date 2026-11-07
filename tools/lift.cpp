#include <cstdlib>
#include "lift.h"

#include <capstone/capstone.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lift
{
namespace
{
csh cs;

enum { CF, PF, AF, ZF, SF, OF, NF };
constexpr unsigned ALL6 = 0x3f;
const char* flagName[] = {"cf", "pf", "af", "zf", "sf", "of"};
const char* regName[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

struct Ins
{
	uint32_t va = 0, next = 0;
	unsigned id = 0;
	std::string mn, ops;
	cs_x86 x{};
	uint8_t size = 0;
	// control flow
	bool terminal = false;       // no fallthrough
	std::vector<uint32_t> succ;  // local successors besides fallthrough
	unsigned rd = 0, wr = 0;     // flag read / write masks
	unsigned liveOut = 0;
	unsigned tailNeed = 0;       // flags the tail-call target reads before writing (handed over through CPU)
};

std::string fmt(const char* f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return buf;
}

std::string hex(uint32_t v) { return fmt("0x%08xu", v); }

const char* ty(int size) { return size == 1 ? "uint8_t" : size == 2 ? "uint16_t" : "uint32_t"; }
const char* sty(int size) { return size == 1 ? "int8_t" : size == 2 ? "int16_t" : "int32_t"; }

struct RegInfo { int idx; int size; bool high; };

bool regInfo(x86_reg r, RegInfo& o)
{
	switch (r)
	{
	case X86_REG_EAX: o = {0, 4, false}; return true; case X86_REG_ECX: o = {1, 4, false}; return true;
	case X86_REG_EDX: o = {2, 4, false}; return true; case X86_REG_EBX: o = {3, 4, false}; return true;
	case X86_REG_ESP: o = {4, 4, false}; return true; case X86_REG_EBP: o = {5, 4, false}; return true;
	case X86_REG_ESI: o = {6, 4, false}; return true; case X86_REG_EDI: o = {7, 4, false}; return true;
	case X86_REG_AX: o = {0, 2, false}; return true; case X86_REG_CX: o = {1, 2, false}; return true;
	case X86_REG_DX: o = {2, 2, false}; return true; case X86_REG_BX: o = {3, 2, false}; return true;
	case X86_REG_SP: o = {4, 2, false}; return true; case X86_REG_BP: o = {5, 2, false}; return true;
	case X86_REG_SI: o = {6, 2, false}; return true; case X86_REG_DI: o = {7, 2, false}; return true;
	case X86_REG_AL: o = {0, 1, false}; return true; case X86_REG_CL: o = {1, 1, false}; return true;
	case X86_REG_DL: o = {2, 1, false}; return true; case X86_REG_BL: o = {3, 1, false}; return true;
	case X86_REG_AH: o = {0, 1, true}; return true; case X86_REG_CH: o = {1, 1, true}; return true;
	case X86_REG_DH: o = {2, 1, true}; return true; case X86_REG_BH: o = {3, 1, true}; return true;
	default: return false;
	}
}

// ---- flag read/write tables ----------------------------------------------------------------

unsigned condMask(unsigned id)
{
	switch (id)
	{
	case X86_INS_JE: case X86_INS_JNE: case X86_INS_SETE: case X86_INS_SETNE: case X86_INS_CMOVE: case X86_INS_CMOVNE: return 1u << ZF;
	case X86_INS_JB: case X86_INS_JAE: case X86_INS_SETB: case X86_INS_SETAE: case X86_INS_CMOVB: case X86_INS_CMOVAE: return 1u << CF;
	case X86_INS_JBE: case X86_INS_JA: case X86_INS_SETBE: case X86_INS_SETA: case X86_INS_CMOVBE: case X86_INS_CMOVA: return (1u << CF) | (1u << ZF);
	case X86_INS_JS: case X86_INS_JNS: case X86_INS_SETS: case X86_INS_SETNS: case X86_INS_CMOVS: case X86_INS_CMOVNS: return 1u << SF;
	case X86_INS_JP: case X86_INS_JNP: case X86_INS_SETP: case X86_INS_SETNP: case X86_INS_CMOVP: case X86_INS_CMOVNP: return 1u << PF;
	case X86_INS_JO: case X86_INS_JNO: case X86_INS_SETO: case X86_INS_SETNO: case X86_INS_CMOVO: case X86_INS_CMOVNO: return 1u << OF;
	case X86_INS_JL: case X86_INS_JGE: case X86_INS_SETL: case X86_INS_SETGE: case X86_INS_CMOVL: case X86_INS_CMOVGE: return (1u << SF) | (1u << OF);
	case X86_INS_JLE: case X86_INS_JG: case X86_INS_SETLE: case X86_INS_SETG: case X86_INS_CMOVLE: case X86_INS_CMOVG: return (1u << ZF) | (1u << SF) | (1u << OF);
	}
	return 0;
}

std::string condExpr(unsigned id)
{
	switch (id)
	{
	case X86_INS_JE: case X86_INS_SETE: case X86_INS_CMOVE: return "zf";
	case X86_INS_JNE: case X86_INS_SETNE: case X86_INS_CMOVNE: return "!zf";
	case X86_INS_JB: case X86_INS_SETB: case X86_INS_CMOVB: return "cf";
	case X86_INS_JAE: case X86_INS_SETAE: case X86_INS_CMOVAE: return "!cf";
	case X86_INS_JBE: case X86_INS_SETBE: case X86_INS_CMOVBE: return "(cf|zf)";
	case X86_INS_JA: case X86_INS_SETA: case X86_INS_CMOVA: return "!(cf|zf)";
	case X86_INS_JS: case X86_INS_SETS: case X86_INS_CMOVS: return "sf";
	case X86_INS_JNS: case X86_INS_SETNS: case X86_INS_CMOVNS: return "!sf";
	case X86_INS_JP: case X86_INS_SETP: case X86_INS_CMOVP: return "pf";
	case X86_INS_JNP: case X86_INS_SETNP: case X86_INS_CMOVNP: return "!pf";
	case X86_INS_JO: case X86_INS_SETO: case X86_INS_CMOVO: return "of";
	case X86_INS_JNO: case X86_INS_SETNO: case X86_INS_CMOVNO: return "!of";
	case X86_INS_JL: case X86_INS_SETL: case X86_INS_CMOVL: return "(sf!=of)";
	case X86_INS_JGE: case X86_INS_SETGE: case X86_INS_CMOVGE: return "(sf==of)";
	case X86_INS_JLE: case X86_INS_SETLE: case X86_INS_CMOVLE: return "(zf|(sf!=of))";
	case X86_INS_JG: case X86_INS_SETG: case X86_INS_CMOVG: return "(!zf&&(sf==of))";
	}
	return "";
}

bool isJcc(unsigned id)
{
	switch (id)
	{
	case X86_INS_JE: case X86_INS_JNE: case X86_INS_JB: case X86_INS_JAE: case X86_INS_JBE: case X86_INS_JA: case X86_INS_JS: case X86_INS_JNS:
	case X86_INS_JP: case X86_INS_JNP: case X86_INS_JO: case X86_INS_JNO: case X86_INS_JL: case X86_INS_JGE: case X86_INS_JLE: case X86_INS_JG:
		return true;
	}
	return false;
}

void flagRW(Ins& i)
{
	i.rd = condMask(i.id);
	i.wr = 0;
	bool variableCount = i.x.op_count >= 2 && i.x.operands[i.x.op_count - 1].type == X86_OP_REG;
	switch (i.id)
	{
	case X86_INS_ADD: case X86_INS_SUB: case X86_INS_CMP: case X86_INS_NEG: case X86_INS_AND: case X86_INS_OR: case X86_INS_XOR:
	case X86_INS_TEST: case X86_INS_XADD: case X86_INS_DIV: case X86_INS_IDIV: case X86_INS_CMPXCHG:
		i.wr = ALL6; break;
	case X86_INS_SAHF: i.wr = ALL6 & ~(1u << OF); break;
	case X86_INS_ADC: case X86_INS_SBB: i.rd = 1u << CF; i.wr = ALL6; break;
	case X86_INS_INC: case X86_INS_DEC: i.wr = ALL6 & ~(1u << CF); break;
	case X86_INS_MUL: case X86_INS_IMUL: i.wr = ALL6; break;
	case X86_INS_SHL: case X86_INS_SAL: case X86_INS_SHR: case X86_INS_SAR: case X86_INS_SHLD: case X86_INS_SHRD:
		if (variableCount) { i.rd = ALL6; }   // count may be 0: flags untouched
		i.wr = ALL6; break;
	case X86_INS_ROL: case X86_INS_ROR:
		if (variableCount) i.rd = (1u << CF) | (1u << OF);
		i.wr = (1u << CF) | (1u << OF); break;
	case X86_INS_RCL: case X86_INS_RCR: i.rd = 1u << CF; i.wr = (1u << CF) | (1u << OF); break;
	case X86_INS_BT: case X86_INS_BTS: case X86_INS_BTR: case X86_INS_BTC: i.wr = ALL6; break;
	case X86_INS_BSF: case X86_INS_BSR: i.wr = ALL6; break;
	case X86_INS_STC: case X86_INS_CLC: i.wr = 1u << CF; break;
	case X86_INS_CMC: i.rd = 1u << CF; i.wr = 1u << CF; break;
	case X86_INS_CMPXCHG8B: i.wr = 1u << ZF; break;
	case X86_INS_LAHF: i.rd = ALL6 & ~(1u << OF); break;
	case X86_INS_PUSHFD: i.rd = ALL6; break;
	case X86_INS_POPFD: i.wr = ALL6; break;
	case X86_INS_SCASB: case X86_INS_SCASW: case X86_INS_SCASD: case X86_INS_CMPSB: case X86_INS_CMPSW: case X86_INS_CMPSD:
		i.wr = ALL6; break;
	case X86_INS_COMISS: case X86_INS_UCOMISS: case X86_INS_FCOMI: case X86_INS_FCOMPI: case X86_INS_FUCOMI: case X86_INS_FUCOMPI:
		i.wr = ALL6; break;
	case X86_INS_CALL: i.wr = ALL6; break;   // flags are dead across calls
	}
}

// ---- operand text ------------------------------------------------------------------------------

struct Emit
{
	Ins* in;
	std::string o;
	unsigned live;       // flags live after this instruction
	bool failed = false;

	Ins& I() { return *in; }

	std::string regRd(const RegInfo& r)
	{
		std::string n = regName[r.idx];
		if (r.size == 4) return n;
		if (r.size == 2) return "(uint16_t)" + n;
		return r.high ? "(uint8_t)(" + n + ">>8)" : "(uint8_t)" + n;
	}

	std::string regWr(const RegInfo& r, const std::string& v)
	{
		std::string n = regName[r.idx];
		if (r.size == 4) return n + " = " + v + ";";
		if (r.size == 2) return n + " = (" + n + " & 0xffff0000u) | (uint16_t)(" + v + ");";
		if (r.high) return n + " = (" + n + " & 0xffff00ffu) | ((uint32_t)(uint8_t)(" + v + ") << 8);";
		return n + " = (" + n + " & 0xffffff00u) | (uint8_t)(" + v + ");";
	}

	std::string ea(const cs_x86_op& op, bool withSeg = true)
	{
		std::string s;
		auto add = [&](const std::string& t) { s += (s.empty() ? "" : " + ") + t; };
		if (op.mem.base != X86_REG_INVALID)
		{
			RegInfo r;
			if (!regInfo((x86_reg)op.mem.base, r)) failed = true; else add(regName[r.idx]);
		}
		if (op.mem.index != X86_REG_INVALID)
		{
			RegInfo r;
			if (!regInfo((x86_reg)op.mem.index, r)) failed = true;
			else add(op.mem.scale == 1 ? std::string(regName[r.idx]) : fmt("%s * %d", regName[r.idx], op.mem.scale));
		}
		if (op.mem.disp || s.empty()) add(hex((uint32_t)op.mem.disp));
		if (withSeg && op.mem.segment == X86_REG_FS) add("c->fsbase");
		else if (withSeg && op.mem.segment != X86_REG_INVALID && op.mem.segment != X86_REG_DS && op.mem.segment != X86_REG_SS
			&& op.mem.segment != X86_REG_ES && op.mem.segment != X86_REG_CS) failed = true;
		return "(uint32_t)(" + s + ")";
	}

	// `eaVar` is a local holding the effective address when the instruction needs it twice.
	std::string rd(const cs_x86_op& op, const char* eaVar = nullptr, int forceSize = 0)
	{
		int size = forceSize ? forceSize : op.size;
		switch (op.type)
		{
		case X86_OP_REG: { RegInfo r; if (!regInfo(op.reg, r)) { failed = true; return "0"; } return regRd(r); }
		case X86_OP_IMM: return fmt("((%s)%s)", ty(size), hex((uint32_t)op.imm).c_str());
		case X86_OP_MEM: return fmt("G%d(%s)", size * 8, eaVar ? eaVar : ea(op).c_str());
		default: failed = true; return "0";
		}
	}

	std::string wr(const cs_x86_op& op, const std::string& v, const char* eaVar = nullptr)
	{
		switch (op.type)
		{
		case X86_OP_REG: { RegInfo r; if (!regInfo(op.reg, r)) { failed = true; return ""; } return regWr(r, v); }
		case X86_OP_MEM: return fmt("S%d(%s, %s);", op.size * 8, eaVar ? eaVar : ea(op).c_str(), v.c_str());
		default: failed = true; return "";
		}
	}

	void line(const std::string& s) { o += "\t" + s + "\n"; }

	void flag(unsigned f, const std::string& expr)
	{
		if (live & (1u << f)) line(std::string(flagName[f]) + " = " + expr + ";");
	}

	void zsp(const std::string& r, int bits)
	{
		flag(ZF, "(" + r + " == 0)");
		flag(SF, fmt("(%s >> %d) & 1", r.c_str(), bits - 1));
		flag(PF, "par8(" + r + ")");
	}
};


// ---- flags handed over by tail jumps ------------------------------------------------------------
// A jump to another function's entry is emitted as a tail call. If the target reads flags before it writes
// them (e.g. "cmp; jbe L" where L starts with "sbb eax, eax") they have to travel through the CPU struct.
Reader* gReader = nullptr;

unsigned entryFlagNeed(uint32_t t)
{
	static std::map<uint32_t, unsigned> cache;
	if (auto it = cache.find(t); it != cache.end()) return it->second;
	unsigned need = 0;
	if (gReader)
	{
		std::set<std::pair<uint32_t, unsigned>> seen;
		std::vector<std::pair<uint32_t, unsigned>> work{{t, 0u}};
		cs_insn* insn = cs_malloc(cs);
		int budget = 400;
		while (!work.empty() && budget > 0)
		{
			auto [pc, written] = work.back();
			work.pop_back();
			for (int n = 0; n < 48 && budget > 0 && gReader->isCode(pc) && seen.insert({pc, written}).second; n++, budget--)
			{
				uint8_t buf[16] = {};
				size_t avail = 0;
				for (; avail < 15 && gReader->read(pc + avail, buf + avail, 1); avail++) {}
				const uint8_t* p = buf;
				size_t left = avail;
				uint64_t a = pc;
				if (!cs_disasm_iter(cs, &p, &left, &a, insn)) break;
				Ins in;
				in.id = insn->id;
				in.x = insn->detail->x86;
				in.next = pc + insn->size;
				flagRW(in);
				need |= in.rd & ~written;
				written |= in.wr;
				if (in.id == X86_INS_CALL || in.id == X86_INS_RET || in.id == X86_INS_INT3 || in.id == X86_INS_UD2 || in.id == X86_INS_HLT) break;
				bool jcc = isJcc(in.id) || in.id == X86_INS_LOOP || in.id == X86_INS_LOOPE || in.id == X86_INS_LOOPNE || in.id == X86_INS_JECXZ;
				if (jcc && in.x.op_count == 1 && in.x.operands[0].type == X86_OP_IMM) work.push_back({(uint32_t)in.x.operands[0].imm, written});
				if (in.id == X86_INS_JMP)
				{
					if (in.x.op_count == 1 && in.x.operands[0].type == X86_OP_IMM) { pc = (uint32_t)in.x.operands[0].imm; continue; }
					break;
				}
				pc = in.next;
			}
		}
		cs_free(insn, 1);
	}
	cache[t] = need;
	return need;
}

// ---- code generation of a single instruction -------------------------------------------------

struct Gen
{
	const std::set<uint32_t>* funcs;
	uint32_t entry;
	std::map<uint32_t, Ins>* code;
	std::vector<std::string> unsup;
	uint32_t setjmpSlot = 0;
	const std::set<uint32_t>* setjmpThunks = nullptr;
	std::vector<uint32_t> sj;   // return addresses of the _setjmp3 call sites

	bool isLocal(uint32_t t) { return code->count(t) && !(t != entry && funcs && funcs->count(t)); }

	std::string jumpTo(uint32_t t, const char* saveRegs = "SAVE();")
	{
		if (isLocal(t)) return fmt("goto L_%08x;", t);
		if (funcs && !funcs->count(t)) return fmt("{ %s call_target(c, 0x%08xu); return; }", saveRegs, t);
		if (entryFlagNeed(t)) return fmt("{ %s SAVEFL(); f_%08x(c); return; }", saveRegs, t);
		return fmt("{ %s f_%08x(c); return; }", saveRegs, t);
	}

	void gen(Ins& in, std::string& out)
	{
		Emit e{&in, "", in.liveOut};
		auto& x = in.x;
		auto op = [&](int i) -> cs_x86_op& { return x.operands[i]; };
		unsigned id = in.id;
		{
			// capstone gives most predicate forms (cmpeqps, cmpleps, ...) ids of their own: dispatch by mnemonic
			const std::string& m = in.mn;
			bool tail = m.size() >= 2 && (m.compare(m.size() - 2, 2, "ps") == 0 || m.compare(m.size() - 2, 2, "ss") == 0);
			if (m.compare(0, 3, "cmp") == 0 && tail && (m.size() >= 7 || x.op_count == 3) && id != X86_INS_CMPPS && id != X86_INS_CMPSS) id = X86_INS_CMPPS;
		}
		int sz = x.op_count ? op(0).size : 4;
		bool ok = true;
		bool lk = x.prefix[0] == X86_PREFIX_LOCK;

		auto alu = [&](const char* kind) {
			// dst = dst <kind> src with flags
			bool mem = op(0).type == X86_OP_MEM;
			std::string t = ty(sz);
			int bits = sz * 8;
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			std::string k = kind;
			bool store = true;
			bool atom = lk && mem && (k == "add" || k == "sub" || k == "and" || k == "or" || k == "xor");
			if (atom)
				e.line("{ " + t + " b = " + e.rd(op(1), nullptr, sz) + "; " + t + " a = __atomic_fetch_" + k + "((" + t + "*)guest::host(ea), b, __ATOMIC_SEQ_CST);");
			else
				e.line("{ " + t + " a = " + e.rd(op(0), mem ? "ea" : nullptr) + ", b = " + e.rd(op(1), nullptr, sz) + ";");
			if (k == "add" || k == "adc")
			{
				if (k == "adc") e.line(t + " r = (" + t + ")(a + b + cf);"); else e.line(t + " r = (" + t + ")(a + b);");
				e.flag(CF, k == "adc" ? "cf ? (r <= a) : (r < a)" : "r < a");
				e.flag(OF, fmt("((a ^ r) & (b ^ r)) >> %d", bits - 1));
				e.flag(AF, "((a ^ b ^ r) >> 4) & 1");
				e.zsp("r", bits);
				if (!atom) e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			}
			else if (k == "sub" || k == "sbb" || k == "cmp")
			{
				if (k == "sbb") e.line(t + " r = (" + t + ")(a - b - cf);"); else e.line(t + " r = (" + t + ")(a - b);");
				e.flag(CF, k == "sbb" ? "cf ? (a <= b) : (a < b)" : "a < b");
				e.flag(OF, fmt("((a ^ b) & (a ^ r)) >> %d", bits - 1));
				e.flag(AF, "((a ^ b ^ r) >> 4) & 1");
				e.zsp("r", bits);
				if (k != "cmp" && !atom) e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			}
			else
			{
				const char* o = k == "and" || k == "test" ? "&" : k == "or" ? "|" : "^";
				e.line(t + " r = (" + t + ")(a " + o + " b);");
				e.flag(CF, "0"); e.flag(OF, "0"); e.flag(AF, "0");
				e.zsp("r", bits);
				if (k != "test" && !atom) e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			}
			(void)store;
			e.line("}");
		};

		auto shift = [&]() {
			bool mem = op(0).type == X86_OP_MEM;
			std::string t = ty(sz);
			int bits = sz * 8;
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			std::string cnt = x.op_count == 1 ? "1" : e.rd(op(1), nullptr, 1);
			e.line("{ " + t + " a = " + e.rd(op(0), mem ? "ea" : nullptr) + "; unsigned n = (" + cnt + ") & 31; if (n) {");
			e.line("" + t + " r;");
			switch (id)
			{
			case X86_INS_SHL:
				e.line("r = (" + t + ")(n >= " + std::to_string(bits) + " ? 0 : (a << n));");
				e.flag(CF, fmt("n > %d ? 0 : (a >> (%d - n)) & 1", bits, bits));
				e.flag(OF, fmt("((r >> %d) & 1) ^ (n > %d ? 0 : (a >> (%d - n)) & 1)", bits - 1, bits, bits));
				break;
			case X86_INS_SHR:
				e.line("r = (" + t + ")(n >= " + std::to_string(bits) + " ? 0 : (a >> n));");
				e.flag(CF, fmt("n > %d ? 0 : (a >> (n - 1)) & 1", bits));
				e.flag(OF, fmt("(a >> %d) & 1", bits - 1));
				break;
			case X86_INS_SAR:
				e.line(std::string("r = (") + t + ")((" + sty(sz) + ")a >> (n >= " + std::to_string(bits) + " ? " + std::to_string(bits - 1) + " : n));");
				e.flag(CF, fmt("((%s)a >> (n > %d ? %d : n - 1)) & 1", sty(sz), bits, bits - 1));
				e.flag(OF, "0");
				break;
			}
			e.flag(AF, "0");
			e.zsp("r", bits);
			e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			e.line("} }");
		};

		auto rotate = [&]() {
			bool mem = op(0).type == X86_OP_MEM;
			std::string t = ty(sz);
			int bits = sz * 8;
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			std::string cnt = x.op_count == 1 ? "1" : e.rd(op(1), nullptr, 1);
			e.line("{ " + t + " a = " + e.rd(op(0), mem ? "ea" : nullptr) + "; unsigned n = (" + cnt + ") & 31; if (n) { unsigned m = n % " + std::to_string(bits) + ";");
			if (id == X86_INS_ROL)
			{
				e.line(t + " r = (" + t + ")(m ? (a << m) | (a >> (" + std::to_string(bits) + " - m)) : a);");
				e.flag(CF, "r & 1");
				e.flag(OF, fmt("((r >> %d) & 1) ^ (r & 1)", bits - 1));
			}
			else
			{
				e.line(t + " r = (" + t + ")(m ? (a >> m) | (a << (" + std::to_string(bits) + " - m)) : a);");
				e.flag(CF, fmt("(r >> %d) & 1", bits - 1));
				e.flag(OF, fmt("((r >> %d) ^ (r >> %d)) & 1", bits - 1, bits - 2));
			}
			e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			e.line("} }");
		};

		auto isX = [&](const cs_x86_op& o) { return o.type == X86_OP_REG && o.reg >= X86_REG_XMM0 && o.reg <= X86_REG_XMM7; };
		auto xi = [&](const cs_x86_op& o) { return (int)(o.reg - X86_REG_XMM0); };
		auto X = [&](const cs_x86_op& o) { return fmt("c->xmm[%d]", xi(o)); };
		auto src128 = [&](const cs_x86_op& o) { return isX(o) ? X(o) : "ld128(" + e.ea(o) + ")"; };
		auto srcF = [&](const cs_x86_op& o) { return isX(o) ? X(o) + ".f[0]" : "ldf32(" + e.ea(o) + ")"; };
		auto isST = [&](const cs_x86_op& o) { return o.type == X86_OP_REG && o.reg >= X86_REG_ST0 && o.reg <= X86_REG_ST7; };
		auto sti = [&](const cs_x86_op& o) { return (int)(o.reg - X86_REG_ST0); };
		auto condJumpOrTail = [&](uint32_t t) {
			std::string c = condExpr(id);
			e.line("if (" + c + ") " + jumpTo(t));
		};

		// ---- MMX: any instruction with an mm register operand (mm registers live in c->mm[])
		bool mmxHandled = false;
		{
			auto isM = [&](const cs_x86_op& o) { return o.type == X86_OP_REG && o.reg >= X86_REG_MM0 && o.reg <= X86_REG_MM7; };
			bool anyM = x.op_count > 0 && (isM(op(0)) || (x.op_count > 1 && isM(op(1))));
			// SSE2 integer forms (xmm operands): lane-wise ops run the MMX helper on each 64-bit half
			bool xmmInt = x.op_count == 2 && isX(op(0)) && (isX(op(1)) || op(1).type == X86_OP_MEM || op(1).type == X86_OP_IMM);
			if (xmmInt)
			{
				const char* lane = nullptr; const char* full = nullptr; bool shiftByte = false;
				switch (id)
				{
#define XLANE(ID, F) case X86_INS_##ID: lane = #F; break;
#define XFULL(ID, F) case X86_INS_##ID: full = #F; break;
				XLANE(PADDB, mm_paddb) XLANE(PADDW, mm_paddw) XLANE(PADDD, mm_paddd) XLANE(PSUBB, mm_psubb) XLANE(PSUBW, mm_psubw) XLANE(PSUBD, mm_psubd)
				XLANE(PADDSB, mm_paddsb) XLANE(PADDSW, mm_paddsw) XLANE(PSUBSB, mm_psubsb) XLANE(PSUBSW, mm_psubsw)
				XLANE(PADDUSB, mm_paddusb) XLANE(PADDUSW, mm_paddusw) XLANE(PSUBUSB, mm_psubusb) XLANE(PSUBUSW, mm_psubusw)
				XLANE(PMULLW, mm_pmullw) XLANE(PMULHW, mm_pmulhw) XLANE(PMULHUW, mm_pmulhuw) XLANE(PMADDWD, mm_pmaddwd)
				XLANE(PCMPEQB, mm_pcmpeqb) XLANE(PCMPEQW, mm_pcmpeqw) XLANE(PCMPEQD, mm_pcmpeqd)
				XLANE(PCMPGTB, mm_pcmpgtb) XLANE(PCMPGTW, mm_pcmpgtw) XLANE(PCMPGTD, mm_pcmpgtd)
				XLANE(PSLLW, mm_psllw) XLANE(PSLLD, mm_pslld) XLANE(PSLLQ, mm_psllq) XLANE(PSRLW, mm_psrlw) XLANE(PSRLD, mm_psrld) XLANE(PSRLQ, mm_psrlq)
				XLANE(PSRAW, mm_psraw) XLANE(PSRAD, mm_psrad)
				XLANE(PMAXSW, mm_pmaxsw) XLANE(PMINSW, mm_pminsw) XLANE(PMAXUB, mm_pmaxub) XLANE(PMINUB, mm_pminub) XLANE(PAVGB, mm_pavgb) XLANE(PAVGW, mm_pavgw) XLANE(PSADBW, mm_psadbw)
				XFULL(PUNPCKLBW, xmm_punpcklbw) XFULL(PUNPCKHBW, xmm_punpckhbw) XFULL(PUNPCKLWD, xmm_punpcklwd) XFULL(PUNPCKHWD, xmm_punpckhwd)
				XFULL(PUNPCKLDQ, xmm_punpckldq) XFULL(PUNPCKHDQ, xmm_punpckhdq) XFULL(PUNPCKLQDQ, xmm_punpcklqdq) XFULL(PUNPCKHQDQ, xmm_punpckhqdq)
				XFULL(PACKSSWB, xmm_packsswb) XFULL(PACKUSWB, xmm_packuswb) XFULL(PACKSSDW, xmm_packssdw)
#undef XLANE
#undef XFULL
				case X86_INS_PSLLDQ: full = "xmm_pslldq"; shiftByte = true; break;
				case X86_INS_PSRLDQ: full = "xmm_psrldq"; shiftByte = true; break;
				default: break;
				}
				bool shift = lane && (id == X86_INS_PSLLW || id == X86_INS_PSLLD || id == X86_INS_PSLLQ || id == X86_INS_PSRLW || id == X86_INS_PSRLD || id == X86_INS_PSRLQ || id == X86_INS_PSRAW || id == X86_INS_PSRAD);
				if (lane)
				{
					std::string d = X(op(0));
					if (shift && op(1).type == X86_OP_IMM) e.line(fmt("{ uint64_t n = %uu; %s.q[0] = %s(%s.q[0], n); %s.q[1] = %s(%s.q[1], n); }", (unsigned)op(1).imm & 0xffu, d.c_str(), lane, d.c_str(), d.c_str(), lane, d.c_str()));
					else if (shift) e.line("{ uint64_t n = " + src128(op(1)) + ".q[0]; " + d + ".q[0] = " + lane + "(" + d + ".q[0], n); " + d + ".q[1] = " + lane + "(" + d + ".q[1], n); }");
					else e.line("{ X128 b = " + src128(op(1)) + "; " + d + ".q[0] = " + lane + "(" + d + ".q[0], b.q[0]); " + d + ".q[1] = " + lane + "(" + d + ".q[1], b.q[1]); }");
					mmxHandled = true;
				}
				else if (full && shiftByte && op(1).type == X86_OP_IMM) { e.line(X(op(0)) + " = " + full + "(" + X(op(0)) + fmt(", %uu);", (unsigned)op(1).imm & 0xffu)); mmxHandled = true; }
				else if (full && !shiftByte) { e.line("{ X128 b = " + src128(op(1)) + "; " + X(op(0)) + " = " + full + "(" + X(op(0)) + ", b); }"); mmxHandled = true; }
			}
			if (mmxHandled) {}
			else if (id == X86_INS_EMMS) { mmxHandled = true; e.line("/* emms */"); }
			else if (anyM)
			{
				auto M = [&](const cs_x86_op& o) { return fmt("c->mm[%d]", (int)(o.reg - X86_REG_MM0)); };
				auto S = [&](const cs_x86_op& o) -> std::string {
					if (isM(o)) return M(o);
					if (o.type == X86_OP_IMM) return fmt("(uint64_t)%uu", (unsigned)o.imm & 0xffu);
					if (o.type == X86_OP_MEM) return "guest::load<uint64_t>(" + e.ea(o) + ")";
					return "0";
				};
				const char* fn = nullptr;
				switch (id)
				{
#define MMOP(ID, F) case X86_INS_##ID: fn = #F; break;
				MMOP(PADDB, mm_paddb) MMOP(PADDW, mm_paddw) MMOP(PADDD, mm_paddd) MMOP(PSUBB, mm_psubb) MMOP(PSUBW, mm_psubw) MMOP(PSUBD, mm_psubd)
				MMOP(PADDSB, mm_paddsb) MMOP(PADDSW, mm_paddsw) MMOP(PSUBSB, mm_psubsb) MMOP(PSUBSW, mm_psubsw)
				MMOP(PADDUSB, mm_paddusb) MMOP(PADDUSW, mm_paddusw) MMOP(PSUBUSB, mm_psubusb) MMOP(PSUBUSW, mm_psubusw)
				MMOP(PMULLW, mm_pmullw) MMOP(PMULHW, mm_pmulhw) MMOP(PMULHUW, mm_pmulhuw) MMOP(PMADDWD, mm_pmaddwd)
				MMOP(PCMPEQB, mm_pcmpeqb) MMOP(PCMPEQW, mm_pcmpeqw) MMOP(PCMPEQD, mm_pcmpeqd)
				MMOP(PCMPGTB, mm_pcmpgtb) MMOP(PCMPGTW, mm_pcmpgtw) MMOP(PCMPGTD, mm_pcmpgtd)
				MMOP(PUNPCKLBW, mm_punpcklbw) MMOP(PUNPCKHBW, mm_punpckhbw) MMOP(PUNPCKLWD, mm_punpcklwd) MMOP(PUNPCKHWD, mm_punpckhwd)
				MMOP(PUNPCKLDQ, mm_punpckldq) MMOP(PUNPCKHDQ, mm_punpckhdq)
				MMOP(PACKSSWB, mm_packsswb) MMOP(PACKUSWB, mm_packuswb) MMOP(PACKSSDW, mm_packssdw)
				MMOP(PSLLW, mm_psllw) MMOP(PSLLD, mm_pslld) MMOP(PSLLQ, mm_psllq) MMOP(PSRLW, mm_psrlw) MMOP(PSRLD, mm_psrld) MMOP(PSRLQ, mm_psrlq)
				MMOP(PSRAW, mm_psraw) MMOP(PSRAD, mm_psrad)
				MMOP(PMAXSW, mm_pmaxsw) MMOP(PMINSW, mm_pminsw) MMOP(PMAXUB, mm_pmaxub) MMOP(PMINUB, mm_pminub) MMOP(PAVGB, mm_pavgb) MMOP(PAVGW, mm_pavgw) MMOP(PSADBW, mm_psadbw)
#undef MMOP
				default: break;
				}
				if (fn && isM(op(0)) && x.op_count == 2)
				{
					e.line("{ uint64_t b = " + S(op(1)) + "; " + M(op(0)) + " = " + fn + "(" + M(op(0)) + ", b); }");
					mmxHandled = true;
				}
				else if ((id == X86_INS_PAND || id == X86_INS_POR || id == X86_INS_PXOR || id == X86_INS_PANDN) && isM(op(0)))
				{
					const char* o = id == X86_INS_PAND ? "a & b" : id == X86_INS_POR ? "a | b" : id == X86_INS_PXOR ? "a ^ b" : "~a & b";
					e.line("{ uint64_t a = " + M(op(0)) + ", b = " + S(op(1)) + "; " + M(op(0)) + " = " + o + "; }");
					mmxHandled = true;
				}
				else if (id == X86_INS_MOVQ)
				{
					if (isM(op(0))) { e.line("{ uint64_t v = " + S(op(1)) + "; " + M(op(0)) + " = v; }"); mmxHandled = true; }
					else if (isM(op(1)) && op(0).type == X86_OP_MEM) { e.line("guest::store<uint64_t>(" + e.ea(op(0)) + ", " + M(op(1)) + ");"); mmxHandled = true; }
				}
				else if (id == X86_INS_MOVD)
				{
					if (isM(op(0))) { e.line("{ uint32_t v = " + e.rd(op(1), nullptr, 4) + "; " + M(op(0)) + " = v; }"); mmxHandled = true; }
					else if (isM(op(1))) { e.line(e.wr(op(0), "(uint32_t)" + M(op(1)))); mmxHandled = true; }
				}
				if (!mmxHandled) { ok = false; mmxHandled = true; }
			}
		}
		if (!mmxHandled) switch (id)
		{
		case X86_INS_NOP: case X86_INS_PAUSE: case X86_INS_FNOP: break;

		case X86_INS_MOV:
			if (op(0).type == X86_OP_REG && op(1).type == X86_OP_REG) { RegInfo a, b; if (!regInfo(op(0).reg, a) || !regInfo(op(1).reg, b)) { ok = false; break; } }
			e.line(e.wr(op(0), e.rd(op(1), nullptr, sz)));
			break;
		case X86_INS_MOVZX:
			e.line(e.wr(op(0), "(uint32_t)" + e.rd(op(1))));
			break;
		case X86_INS_MOVSX:
			if (sz == 4) e.line(e.wr(op(0), fmt("(uint32_t)(int32_t)(%s)%s", sty(op(1).size), e.rd(op(1)).c_str())));
			else e.line(e.wr(op(0), fmt("(uint16_t)(int16_t)(%s)%s", sty(op(1).size), e.rd(op(1)).c_str())));
			break;
		case X86_INS_LEA:
			e.line(e.wr(op(0), e.ea(op(1), false)));
			break;
		case X86_INS_XCHG:
			if (op(0).type == X86_OP_MEM || op(1).type == X86_OP_MEM)
			{
				int mi = op(0).type == X86_OP_MEM ? 0 : 1;
				e.line("{ uint32_t ea = " + e.ea(op(mi)) + ";");
				e.line(std::string(ty(sz)) + " t = __atomic_exchange_n((" + ty(sz) + "*)guest::host(ea), " + e.rd(op(1 - mi)) + ", __ATOMIC_SEQ_CST);");   // xchg with memory is always locked
				e.line(e.wr(op(1 - mi), "t"));
				e.line("}");
			}
			else
			{
				e.line("{ " + std::string(ty(sz)) + " t = " + e.rd(op(0)) + ";");
				e.line(e.wr(op(0), e.rd(op(1))));
				e.line(e.wr(op(1), "t"));
				e.line("}");
			}
			break;
		case X86_INS_XADD:
		{
			if (op(0).type != X86_OP_MEM) { ok = false; break; }
			std::string t = ty(sz);
			int bits = sz * 8;
			e.line("{ uint32_t ea = " + e.ea(op(0)) + "; " + t + " b = " + e.rd(op(1)) + "; " + t + " a = __atomic_fetch_add((" + t + "*)guest::host(ea), b, __ATOMIC_SEQ_CST); " + t + " r = (" + t + ")(a + b);");
			e.flag(CF, "r < a");
			e.flag(OF, fmt("((a ^ r) & (b ^ r)) >> %d", bits - 1));
			e.flag(AF, "((a ^ b ^ r) >> 4) & 1");
			e.zsp("r", bits);
			e.line(e.wr(op(1), "a"));
			e.line("}");
			break;
		}
		case X86_INS_CMPXCHG:
		{
			if (op(0).type != X86_OP_MEM) { ok = false; break; }
			std::string t = ty(sz);
			int bits = sz * 8;
			std::string acc = sz == 4 ? "eax" : sz == 2 ? "(uint16_t)eax" : "(uint8_t)eax";
			e.line("{ uint32_t ea = " + e.ea(op(0)) + "; " + t + " a = " + acc + ", exp = a, nv = " + e.rd(op(1)) + ";");
			e.line("bool hit = __atomic_compare_exchange_n((" + t + "*)guest::host(ea), &exp, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);");
			e.line(t + " b = exp, r = (" + t + ")(a - b);");   // on a hit exp == a
			e.flag(CF, "a < b");
			e.flag(OF, fmt("((a ^ b) & (a ^ r)) >> %d", bits - 1));
			e.flag(AF, "((a ^ b ^ r) >> 4) & 1");
			e.zsp("r", bits);
			e.line("if (!hit) " + std::string(sz == 4 ? "eax = exp;" : sz == 2 ? "eax = (eax & 0xffff0000u) | exp;" : "eax = (eax & 0xffffff00u) | exp;"));
			e.line("}");
			break;
		}

		case X86_INS_CMPXCHG8B:
		{
			if (op(0).type != X86_OP_MEM) { ok = false; break; }
			e.line("{ uint32_t ea = " + e.ea(op(0)) + "; uint64_t exp = ((uint64_t)edx << 32) | eax, nv = ((uint64_t)ecx << 32) | ebx;");
			e.line("bool hit = __atomic_compare_exchange_n((uint64_t*)guest::host(ea), &exp, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);");
			e.flag(ZF, "hit");
			e.line("if (!hit) { eax = (uint32_t)exp; edx = (uint32_t)(exp >> 32); } }");
			break;
		}

		case X86_INS_PUSH:
			if (sz != 4) { ok = false; break; }
			e.line("{ uint32_t v = " + e.rd(op(0), nullptr, 4) + "; esp -= 4; S32(esp, v); }");
			break;
		case X86_INS_POP:
			if (sz != 4) { ok = false; break; }
			e.line("{ uint32_t v = G32(esp); esp += 4; " + e.wr(op(0), "v") + " }");
			break;
		case X86_INS_PUSHAL:
			e.line("{ uint32_t t = esp; esp -= 4; S32(esp, eax); esp -= 4; S32(esp, ecx); esp -= 4; S32(esp, edx); esp -= 4; S32(esp, ebx); esp -= 4; S32(esp, t); esp -= 4; S32(esp, ebp); esp -= 4; S32(esp, esi); esp -= 4; S32(esp, edi); }");
			break;
		case X86_INS_POPAL:
			e.line("{ edi = G32(esp); esp += 4; esi = G32(esp); esp += 4; ebp = G32(esp); esp += 8; ebx = G32(esp); esp += 4; edx = G32(esp); esp += 4; ecx = G32(esp); esp += 4; eax = G32(esp); esp += 4; }");
			break;
		case X86_INS_LEAVE: e.line("esp = ebp; ebp = G32(esp); esp += 4;"); break;

		case X86_INS_ADD: alu("add"); break;
		case X86_INS_ADC: alu("adc"); break;
		case X86_INS_SUB: alu("sub"); break;
		case X86_INS_SBB: alu("sbb"); break;
		case X86_INS_CMP: alu("cmp"); break;
		case X86_INS_AND: alu("and"); break;
		case X86_INS_OR: alu("or"); break;
		case X86_INS_XOR: alu("xor"); break;
		case X86_INS_TEST: alu("test"); break;

		case X86_INS_INC: case X86_INS_DEC:
		{
			bool mem = op(0).type == X86_OP_MEM;
			std::string t = ty(sz);
			int bits = sz * 8;
			bool inc = id == X86_INS_INC;
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			bool atom = lk && mem;
			if (atom) e.line("{ " + t + " a = __atomic_fetch_" + (inc ? "add" : "sub") + "((" + t + "*)guest::host(ea), 1, __ATOMIC_SEQ_CST); " + t + " r = (" + t + ")(a " + (inc ? "+" : "-") + " 1);");
			else e.line("{ " + t + " a = " + e.rd(op(0), mem ? "ea" : nullptr) + "; " + t + " r = (" + t + ")(a " + (inc ? "+" : "-") + " 1);");
			e.flag(OF, inc ? fmt("r == (%s)(1u << %d)", t.c_str(), bits - 1) : fmt("a == (%s)(1u << %d)", t.c_str(), bits - 1));
			e.flag(AF, "((a ^ 1 ^ r) >> 4) & 1");
			e.zsp("r", bits);
			if (!atom) e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			e.line("}");
			break;
		}
		case X86_INS_NEG:
		{
			bool mem = op(0).type == X86_OP_MEM;
			std::string t = ty(sz);
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			e.line("{ " + t + " a = " + e.rd(op(0), mem ? "ea" : nullptr) + "; " + t + " r = (" + t + ")(0 - a);");
			e.flag(CF, "a != 0");
			e.flag(OF, fmt("a == (%s)(1u << %d)", t.c_str(), sz * 8 - 1));
			e.flag(AF, "((a ^ r) >> 4) & 1");
			e.zsp("r", sz * 8);
			e.line(e.wr(op(0), "r", mem ? "ea" : nullptr));
			e.line("}");
			break;
		}
		case X86_INS_NOT:
		{
			bool mem = op(0).type == X86_OP_MEM;
			if (mem) e.line("uint32_t ea = " + e.ea(op(0)) + ";");
			e.line(e.wr(op(0), "~" + e.rd(op(0), mem ? "ea" : nullptr), mem ? "ea" : nullptr));
			break;
		}
		case X86_INS_SHL: case X86_INS_SHR: case X86_INS_SAR: shift(); break;
		case X86_INS_ROL: case X86_INS_ROR: rotate(); break;

		case X86_INS_BSWAP: e.line(e.wr(op(0), "__builtin_bswap32(" + e.rd(op(0)) + ")")); break;

		case X86_INS_CDQ: e.line("edx = (uint32_t)((int32_t)eax >> 31);"); break;
		case X86_INS_CWDE: e.line("eax = (uint32_t)(int32_t)(int16_t)eax;"); break;
		case X86_INS_CBW: e.line("eax = (eax & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)eax;"); break;
		case X86_INS_CWD: e.line("edx = (edx & 0xffff0000u) | (uint16_t)((int16_t)eax >> 15);"); break;

		case X86_INS_MUL: case X86_INS_IMUL:
		{
			bool sg = id == X86_INS_IMUL;
			if (x.op_count == 1)
			{
				if (sz == 4)
				{
					e.line(std::string("{ ") + (sg ? "int64_t p = (int64_t)(int32_t)eax * (int32_t)" : "uint64_t p = (uint64_t)eax * (uint32_t)") + e.rd(op(0)) + ";");
					e.line("eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);");
					e.flag(CF, sg ? "p != (int64_t)(int32_t)p" : "edx != 0"); e.flag(OF, sg ? "p != (int64_t)(int32_t)p" : "edx != 0");
					e.line("}");
				}
				else if (sz == 1)
				{
					e.line(std::string("{ ") + (sg ? "int32_t p = (int8_t)eax * (int8_t)" : "uint32_t p = (uint8_t)eax * (uint8_t)") + e.rd(op(0)) + ";");
					e.line("eax = (eax & 0xffff0000u) | (uint16_t)p;");
					e.flag(CF, sg ? "p != (int8_t)p" : "(p >> 8) != 0"); e.flag(OF, sg ? "p != (int8_t)p" : "(p >> 8) != 0");
					e.line("}");
				}
				else
				{
					e.line(std::string("{ ") + (sg ? "int32_t p = (int16_t)eax * (int16_t)" : "uint32_t p = (uint16_t)eax * (uint16_t)") + e.rd(op(0)) + ";");
					e.line("eax = (eax & 0xffff0000u) | (uint16_t)p; edx = (edx & 0xffff0000u) | (uint16_t)((uint32_t)p >> 16);");
					e.flag(CF, sg ? "p != (int16_t)p" : "(p >> 16) != 0"); e.flag(OF, sg ? "p != (int16_t)p" : "(p >> 16) != 0");
					e.line("}");
				}
			}
			else
			{
				if (sz != 4) { ok = false; break; }
				// imul r, r/m   or   imul r, r/m, imm
				std::string a = x.op_count == 2 ? e.rd(op(0)) : e.rd(op(1));
				std::string b = x.op_count == 2 ? e.rd(op(1)) : fmt("(uint32_t)%s", hex((uint32_t)op(2).imm).c_str());
				e.line("{ int64_t p = (int64_t)(int32_t)" + a + " * (int32_t)" + b + ";");
				e.flag(CF, "p != (int64_t)(int32_t)p"); e.flag(OF, "p != (int64_t)(int32_t)p");
				e.line(e.wr(op(0), "(uint32_t)p"));
				e.line("}");
			}
			break;
		}
		case X86_INS_DIV: case X86_INS_IDIV:
		{
			bool sg = id == X86_INS_IDIV;
			if (sz == 4)
			{
				e.line("{ " + std::string(sg ? "int32_t" : "uint32_t") + " d = " + e.rd(op(0)) + "; if (!d) unsupported(c, " + hex(in.va) + ", \"divide by zero\");");
				if (sg)
					e.line("int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }");
				else
					e.line("uint64_t n = ((uint64_t)edx << 32) | eax; eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }");
			}
			else if (sz == 1)
			{
				e.line("{ " + std::string(sg ? "int32_t" : "uint32_t") + " d = " + (sg ? "(int8_t)" : "(uint8_t)") + e.rd(op(0)) + "; if (!d) unsupported(c, " + hex(in.va) + ", \"divide by zero\");");
				e.line(std::string(sg ? "int32_t n = (int16_t)eax;" : "uint32_t n = (uint16_t)eax;") + " uint32_t q = (uint32_t)(n / d), r = (uint32_t)(n % d);");
				e.line("eax = (eax & 0xffff0000u) | ((r & 0xff) << 8) | (q & 0xff); }");
			}
			else
			{
				e.line("{ " + std::string(sg ? "int32_t" : "uint32_t") + " d = " + (sg ? "(int16_t)" : "(uint16_t)") + e.rd(op(0)) + "; if (!d) unsupported(c, " + hex(in.va) + ", \"divide by zero\");");
				e.line(std::string(sg ? "int32_t n = (int32_t)(((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax);" : "uint32_t n = ((uint32_t)(uint16_t)edx << 16) | (uint16_t)eax;") + " uint32_t q = (uint32_t)(n / d), r = (uint32_t)(n % d);");
				e.line("eax = (eax & 0xffff0000u) | (q & 0xffff); edx = (edx & 0xffff0000u) | (r & 0xffff); }");
			}
			break;
		}

		case X86_INS_CLC: e.flag(CF, "0"); break;
		case X86_INS_STC: e.flag(CF, "1"); break;
		case X86_INS_CMC: e.flag(CF, "!cf"); break;
		case X86_INS_CLD: e.line("df = 0;"); break;
		case X86_INS_STD: e.line("df = 1;"); break;
		case X86_INS_SAHF:
			e.flag(CF, "(eax >> 8) & 1"); e.flag(PF, "(eax >> 10) & 1"); e.flag(AF, "(eax >> 12) & 1");
			e.flag(ZF, "(eax >> 14) & 1"); e.flag(SF, "(eax >> 15) & 1");
			break;
		case X86_INS_PUSHFD:
			e.line("esp -= 4; S32(esp, (uint32_t)(cf | pf << 2 | af << 4 | zf << 6 | sf << 7 | df << 10 | of << 11) | 0x202u);");
			break;
		case X86_INS_POPFD:
			e.line("{ uint32_t fl = G32(esp); esp += 4; df = (fl >> 10) & 1;");
			e.flag(CF, "fl & 1"); e.flag(PF, "(fl >> 2) & 1"); e.flag(AF, "(fl >> 4) & 1");
			e.flag(ZF, "(fl >> 6) & 1"); e.flag(SF, "(fl >> 7) & 1"); e.flag(OF, "(fl >> 11) & 1");
			e.line("}");
			break;
		case X86_INS_LAHF:
			e.line("eax = (eax & 0xffff00ffu) | ((uint32_t)(sf << 7 | zf << 6 | af << 4 | pf << 2 | 2 | cf) << 8);");
			break;

		case X86_INS_BSF: case X86_INS_BSR:
			e.line("{ uint32_t v = " + e.rd(op(1)) + ";");
			e.flag(ZF, "v == 0");
			e.line("if (v) " + e.wr(op(0), id == X86_INS_BSF ? "(uint32_t)__builtin_ctz(v)" : "(uint32_t)(31 - __builtin_clz(v))"));
			e.line("}");
			break;
		case X86_INS_BT: case X86_INS_BTS: case X86_INS_BTR: case X86_INS_BTC:
			if (op(0).type != X86_OP_REG || sz != 4) { ok = false; break; }
			e.line("{ unsigned n = " + e.rd(op(1), nullptr, 4) + " & 31;");
			e.flag(CF, e.rd(op(0)) + " >> n & 1");
			if (id == X86_INS_BTS) e.line(e.wr(op(0), e.rd(op(0)) + " | (1u << n)"));
			if (id == X86_INS_BTR) e.line(e.wr(op(0), e.rd(op(0)) + " & ~(1u << n)"));
			if (id == X86_INS_BTC) e.line(e.wr(op(0), e.rd(op(0)) + " ^ (1u << n)"));
			e.line("}");
			break;

		case X86_INS_SETE: case X86_INS_SETNE: case X86_INS_SETB: case X86_INS_SETAE: case X86_INS_SETBE: case X86_INS_SETA:
		case X86_INS_SETS: case X86_INS_SETNS: case X86_INS_SETP: case X86_INS_SETNP: case X86_INS_SETO: case X86_INS_SETNO:
		case X86_INS_SETL: case X86_INS_SETGE: case X86_INS_SETLE: case X86_INS_SETG:
			e.line(e.wr(op(0), "(" + condExpr(id) + ") ? 1 : 0"));
			break;
		case X86_INS_CMOVE: case X86_INS_CMOVNE: case X86_INS_CMOVB: case X86_INS_CMOVAE: case X86_INS_CMOVBE: case X86_INS_CMOVA:
		case X86_INS_CMOVS: case X86_INS_CMOVNS: case X86_INS_CMOVP: case X86_INS_CMOVNP: case X86_INS_CMOVO: case X86_INS_CMOVNO:
		case X86_INS_CMOVL: case X86_INS_CMOVGE: case X86_INS_CMOVLE: case X86_INS_CMOVG:
			e.line("{ " + std::string(ty(sz)) + " v = " + e.rd(op(1)) + "; if (" + condExpr(id) + ") " + e.wr(op(0), "v") + " }");
			break;

		// ---- string instructions
		case X86_INS_MOVSB: case X86_INS_MOVSW: case X86_INS_MOVSD: case X86_INS_STOSB: case X86_INS_STOSW: case X86_INS_STOSD:
		case X86_INS_LODSB: case X86_INS_LODSW: case X86_INS_LODSD: case X86_INS_SCASB: case X86_INS_SCASW: case X86_INS_SCASD:
		case X86_INS_CMPSB: case X86_INS_CMPSW: case X86_INS_CMPSD:
		{
			// Only the integer forms: capstone reuses MOVSD/CMPSD for SSE2, which have explicit xmm operands.
			if (x.op_count && (op(0).type == X86_OP_REG && op(0).reg >= X86_REG_XMM0 && op(0).reg <= X86_REG_XMM31)) { ok = false; break; }
			int w = (id == X86_INS_MOVSB || id == X86_INS_STOSB || id == X86_INS_LODSB || id == X86_INS_SCASB || id == X86_INS_CMPSB) ? 1
				: (id == X86_INS_MOVSW || id == X86_INS_STOSW || id == X86_INS_LODSW || id == X86_INS_SCASW || id == X86_INS_CMPSW) ? 2 : 4;
			bool rep = x.prefix[0] == X86_PREFIX_REP, repne = x.prefix[0] == X86_PREFIX_REPNE;
			std::string t = ty(w);
			std::string step = "(df ? -" + std::to_string(w) + " : " + std::to_string(w) + ")";
			std::string accName = w == 4 ? "eax" : w == 2 ? "(uint16_t)eax" : "(uint8_t)eax";
			std::string body, accWrite;
			bool cmp = false;
			switch (id)
			{
			case X86_INS_MOVSB: case X86_INS_MOVSW: case X86_INS_MOVSD:
				body = fmt("S%d(edi, G%d(esi)); esi += %s; edi += %s;", w * 8, w * 8, step.c_str(), step.c_str()); break;
			case X86_INS_STOSB: case X86_INS_STOSW: case X86_INS_STOSD:
				body = fmt("S%d(edi, %s); edi += %s;", w * 8, accName.c_str(), step.c_str()); break;
			case X86_INS_LODSB: case X86_INS_LODSW: case X86_INS_LODSD:
				body = (w == 4 ? std::string("eax = G32(esi);") : w == 2 ? "eax = (eax & 0xffff0000u) | G16(esi);" : "eax = (eax & 0xffffff00u) | G8(esi);") + " esi += " + step + ";"; break;
			case X86_INS_SCASB: case X86_INS_SCASW: case X86_INS_SCASD:
				cmp = true;
				body = fmt("a = %s; b = G%d(edi); edi += %s;", accName.c_str(), w * 8, step.c_str()); break;
			default:
				cmp = true;
				body = fmt("a = G%d(esi); b = G%d(edi); esi += %s; edi += %s;", w * 8, w * 8, step.c_str(), step.c_str()); break;
			}
			std::string flagCode;
			if (cmp)
			{
				e.live = ALL6;   // loop-carried: keep flags simple and always correct
				Emit f{&in, "", ALL6};
				f.line(t + " r = (" + t + ")(a - b);");
				f.flag(CF, "a < b"); f.flag(OF, fmt("((a ^ b) & (a ^ r)) >> %d", w * 8 - 1)); f.flag(AF, "((a ^ b ^ r) >> 4) & 1"); f.zsp("r", w * 8);
				flagCode = f.o;
			}
			e.line("{");
			if (cmp) e.line(t + " a, b;");
			if (rep || repne)
			{
				e.line("while (ecx) {");
				e.line(body);
				e.line(cmp ? "" : "");
				if (cmp) e.o += flagCode;
				e.line("ecx--;");
				if (cmp) e.line(std::string("if (") + (repne ? "zf" : "!zf") + ") break;");
				e.line("}");
			}
			else
			{
				e.line(body);
				if (cmp) e.o += flagCode;
			}
			e.line("}");
			break;
		}

		case X86_INS_SAL: id = X86_INS_SHL; in.id = id; shift(); break;

		case X86_INS_SHLD: case X86_INS_SHRD:
		{
			if (sz != 4 || op(0).type != X86_OP_REG) { ok = false; break; }
			bool l = id == X86_INS_SHLD;
			e.line("{ uint32_t a = " + e.rd(op(0)) + ", b = " + e.rd(op(1)) + "; unsigned n = (" + e.rd(op(2), nullptr, 1) + ") & 31; if (n) {");
			e.line(l ? "uint32_t r = (a << n) | (b >> (32 - n));" : "uint32_t r = (a >> n) | (b << (32 - n));");
			e.flag(CF, l ? "(a >> (32 - n)) & 1" : "(a >> (n - 1)) & 1");
			e.flag(OF, "((r ^ a) >> 31) & 1");
			e.flag(AF, "0");
			e.zsp("r", 32);
			e.line(e.wr(op(0), "r"));
			e.line("} }");
			break;
		}
		case X86_INS_LOOP: case X86_INS_LOOPE: case X86_INS_LOOPNE:
		{
			std::string c = id == X86_INS_LOOP ? "ecx != 0" : id == X86_INS_LOOPE ? "ecx != 0 && zf" : "ecx != 0 && !zf";
			e.line("ecx--;");
			e.line("if (" + c + ") " + jumpTo((uint32_t)op(0).imm));
			break;
		}
		case X86_INS_JECXZ: e.line("if (ecx == 0) " + jumpTo((uint32_t)op(0).imm)); break;
		case X86_INS_RDTSC: e.line("{ uint64_t t = rdtsc_host(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }"); break;
		case X86_INS_CPUID: e.line("cpuid_impl(eax, ebx, ecx, edx);"); break;
		case X86_INS_WAIT: case X86_INS_FNCLEX: case X86_INS_FFREE: case X86_INS_PREFETCHT0: case X86_INS_PREFETCHT1: case X86_INS_PREFETCHT2:
		case X86_INS_PREFETCHNTA: case X86_INS_EMMS: case X86_INS_SFENCE: case X86_INS_LFENCE: case X86_INS_MFENCE:
			break;

		// ---- SSE (single precision) and the SSE2 moves the game uses
		case X86_INS_MOVSS:
			if (isX(op(0)) && isX(op(1))) e.line(X(op(0)) + ".f[0] = " + X(op(1)) + ".f[0];");
			else if (isX(op(0))) e.line("{ float v = " + srcF(op(1)) + "; " + X(op(0)) + ".f[0] = v; " + X(op(0)) + ".u[1] = 0; " + X(op(0)) + ".u[2] = 0; " + X(op(0)) + ".u[3] = 0; }");
			else if (isX(op(1))) e.line("stf32(" + e.ea(op(0)) + ", " + X(op(1)) + ".f[0]);");
			else ok = false;
			break;
		case X86_INS_MOVAPS: case X86_INS_MOVUPS: case X86_INS_MOVDQA: case X86_INS_MOVDQU: case X86_INS_MOVAPD: case X86_INS_MOVUPD:
			if (isX(op(0))) e.line("{ X128 v = " + src128(op(1)) + "; " + X(op(0)) + " = v; }");
			else if (isX(op(1))) e.line("st128(" + e.ea(op(0)) + ", " + X(op(1)) + ");");
			else ok = false;
			break;
		case X86_INS_MOVLPS: case X86_INS_MOVHPS:
		{
			int lane = id == X86_INS_MOVLPS ? 0 : 1;
			if (isX(op(0)) && op(1).type == X86_OP_MEM) e.line(fmt("%s.q[%d] = guest::load<uint64_t>(%s);", X(op(0)).c_str(), lane, e.ea(op(1)).c_str()));
			else if (isX(op(1)) && op(0).type == X86_OP_MEM) e.line(fmt("guest::store<uint64_t>(%s, %s.q[%d]);", e.ea(op(0)).c_str(), X(op(1)).c_str(), lane));
			else ok = false;
			break;
		}
		case X86_INS_MOVLHPS: e.line(X(op(0)) + ".q[1] = " + X(op(1)) + ".q[0];"); break;
		case X86_INS_MOVHLPS: e.line(X(op(0)) + ".q[0] = " + X(op(1)) + ".q[1];"); break;
		case X86_INS_MOVD:
			if (isX(op(0))) e.line("{ uint32_t v = " + e.rd(op(1), nullptr, 4) + "; " + X(op(0)) + ".u[0] = v; " + X(op(0)) + ".u[1] = 0; " + X(op(0)) + ".q[1] = 0; }");
			else if (isX(op(1))) e.line(e.wr(op(0), X(op(1)) + ".u[0]"));
			else ok = false;
			break;
		case X86_INS_MOVQ:
			if (isX(op(0)) && isX(op(1))) e.line("{ uint64_t v = " + X(op(1)) + ".q[0]; " + X(op(0)) + ".q[0] = v; " + X(op(0)) + ".q[1] = 0; }");
			else if (isX(op(0)) && op(1).type == X86_OP_MEM) e.line("{ uint64_t v = guest::load<uint64_t>(" + e.ea(op(1)) + "); " + X(op(0)) + ".q[0] = v; " + X(op(0)) + ".q[1] = 0; }");
			else if (isX(op(1)) && op(0).type == X86_OP_MEM) e.line("guest::store<uint64_t>(" + e.ea(op(0)) + ", " + X(op(1)) + ".q[0]);");
			else ok = false;
			break;
		case X86_INS_ADDSS: case X86_INS_SUBSS: case X86_INS_MULSS: case X86_INS_DIVSS: case X86_INS_MINSS: case X86_INS_MAXSS: case X86_INS_SQRTSS:
		{
			std::string d = X(op(0)) + ".f[0]", s = srcF(op(1));
			std::string r = id == X86_INS_ADDSS ? d + " + b" : id == X86_INS_SUBSS ? d + " - b" : id == X86_INS_MULSS ? d + " * b" : id == X86_INS_DIVSS ? d + " / b"
				: id == X86_INS_MINSS ? "sse_min(" + d + ", b)" : id == X86_INS_MAXSS ? "sse_max(" + d + ", b)" : "sqrtf(b)";
			e.line("{ float b = " + s + "; " + d + " = " + r + "; }");
			break;
		}
		case X86_INS_ADDPS: case X86_INS_SUBPS: case X86_INS_MULPS: case X86_INS_DIVPS: case X86_INS_MINPS: case X86_INS_MAXPS:
		case X86_INS_SQRTPS: case X86_INS_RCPPS: case X86_INS_RSQRTPS:
		{
			std::string d = X(op(0)) + ".f[k]";
			std::string r = id == X86_INS_ADDPS ? d + " + b.f[k]" : id == X86_INS_SUBPS ? d + " - b.f[k]" : id == X86_INS_MULPS ? d + " * b.f[k]" : id == X86_INS_DIVPS ? d + " / b.f[k]"
				: id == X86_INS_MINPS ? "sse_min(" + d + ", b.f[k])" : id == X86_INS_MAXPS ? "sse_max(" + d + ", b.f[k])" : id == X86_INS_SQRTPS ? "sqrtf(b.f[k])"
				: id == X86_INS_RCPPS ? "1.0f / b.f[k]" : "1.0f / sqrtf(b.f[k])";
			e.line("{ X128 b = " + src128(op(1)) + "; for (int k = 0; k < 4; k++) " + d + " = " + r + "; }");
			break;
		}
		case X86_INS_RSQRTSS: e.line("{ float b = " + srcF(op(1)) + "; " + X(op(0)) + ".f[0] = 1.0f / sqrtf(b); }"); break;
		case X86_INS_ANDPS: case X86_INS_ORPS: case X86_INS_XORPS: case X86_INS_ANDNPS: case X86_INS_PXOR: case X86_INS_PAND: case X86_INS_POR: case X86_INS_PANDN:
		{
			if (!isX(op(0))) { ok = false; break; }
			const char* o = (id == X86_INS_ANDPS || id == X86_INS_PAND) ? "a & b.u[k]" : (id == X86_INS_ORPS || id == X86_INS_POR) ? "a | b.u[k]"
				: (id == X86_INS_ANDNPS || id == X86_INS_PANDN) ? "~a & b.u[k]" : "a ^ b.u[k]";
			e.line("{ X128 b = " + src128(op(1)) + "; for (int k = 0; k < 4; k++) { uint32_t a = " + X(op(0)) + ".u[k]; " + X(op(0)) + ".u[k] = " + o + "; } }");
			break;
		}
		case X86_INS_COMISS: case X86_INS_UCOMISS:
			e.line("{ unsigned fl = fcmpflags(" + srcF(op(0)) + ", " + srcF(op(1)) + ");");
			e.flag(CF, "fl & 1"); e.flag(PF, "(fl >> 2) & 1"); e.flag(ZF, "(fl >> 6) & 1"); e.flag(OF, "0"); e.flag(SF, "0"); e.flag(AF, "0");
			e.line("}");
			break;
		case X86_INS_CVTSI2SS: e.line(X(op(0)) + ".f[0] = (float)(int32_t)" + e.rd(op(1), nullptr, 4) + ";"); break;
		case X86_INS_CVTTSS2SI: e.line(e.wr(op(0), "(uint32_t)sse_cvtt(" + srcF(op(1)) + ")")); break;
		case X86_INS_CVTSS2SI: e.line(e.wr(op(0), "(uint32_t)sse_cvt(" + srcF(op(1)) + ")")); break;
		case X86_INS_SHUFPS:
		{
			unsigned im = (unsigned)op(2).imm & 0xff;
			e.line("{ X128 d = " + X(op(0)) + ", s = " + src128(op(1)) + ";");
			e.line(fmt("%s.f[0] = d.f[%u]; %s.f[1] = d.f[%u]; %s.f[2] = s.f[%u]; %s.f[3] = s.f[%u]; }",
				X(op(0)).c_str(), im & 3, X(op(0)).c_str(), (im >> 2) & 3, X(op(0)).c_str(), (im >> 4) & 3, X(op(0)).c_str(), (im >> 6) & 3));
			break;
		}
		case X86_INS_UNPCKLPS: case X86_INS_UNPCKHPS:
		{
			int b0 = id == X86_INS_UNPCKLPS ? 0 : 2;
			e.line("{ X128 d = " + X(op(0)) + ", s = " + src128(op(1)) + ";");
			e.line(fmt("%s.f[0] = d.f[%d]; %s.f[1] = s.f[%d]; %s.f[2] = d.f[%d]; %s.f[3] = s.f[%d]; }",
				X(op(0)).c_str(), b0, X(op(0)).c_str(), b0, X(op(0)).c_str(), b0 + 1, X(op(0)).c_str(), b0 + 1));
			break;
		}
		case X86_INS_CMPPS: case X86_INS_CMPSS:
		{
			// capstone names the predicate in the mnemonic: cmpeqps, cmpltss, ...
			static const char* preds[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"};
			std::string m = in.mn, name = m.substr(3, m.size() - 5);
			int pr = -1;
			for (int k = 0; k < 8; k++) if (name == preds[k]) pr = k;
			if (pr < 0 && x.op_count == 3) pr = (int)op(2).imm & 7;
			if (pr < 0 || m.size() < 5) { ok = false; break; }
			bool scalar = m.back() == 's' && m[m.size() - 2] == 's';
			if (scalar) e.line("{ float b = " + srcF(op(1)) + "; " + X(op(0)) + fmt(".u[0] = sse_cmp(%d, ", pr) + X(op(0)) + ".f[0], b); }");
			else e.line("{ X128 b = " + src128(op(1)) + "; for (int k = 0; k < 4; k++) " + X(op(0)) + fmt(".u[k] = sse_cmp(%d, ", pr) + X(op(0)) + ".f[k], b.f[k]); }");
			break;
		}
		case X86_INS_MOVMSKPS:
			e.line(e.wr(op(0), "(" + X(op(1)) + ".u[0] >> 31) | ((" + X(op(1)) + ".u[1] >> 31) << 1) | ((" + X(op(1)) + ".u[2] >> 31) << 2) | ((" + X(op(1)) + ".u[3] >> 31) << 3)"));
			break;

		// ---- x87
		case X86_INS_FLD:
			if (isST(op(0))) e.line(fmt("{ double v = ST(c, %d); fpush(c, v); }", sti(op(0))));
			else if (op(0).size == 4) e.line("fpush(c, (double)ldf32(" + e.ea(op(0)) + "));");
			else if (op(0).size == 8) e.line("fpush(c, ldf64(" + e.ea(op(0)) + "));");
			else ok = false;
			break;
		case X86_INS_FILD:
			if (op(0).size == 2) e.line("fpush(c, (double)(int16_t)G16(" + e.ea(op(0)) + "));");
			else if (op(0).size == 4) e.line("fpush(c, (double)(int32_t)G32(" + e.ea(op(0)) + "));");
			else e.line("fpush(c, (double)guest::load<int64_t>(" + e.ea(op(0)) + "));");
			break;
		case X86_INS_FLD1: e.line("fpush(c, 1.0);"); break;
		case X86_INS_FLDZ: e.line("fpush(c, 0.0);"); break;
		case X86_INS_FLDPI: e.line("fpush(c, 3.14159265358979323846);"); break;
		case X86_INS_FLDLN2: e.line("fpush(c, 0.693147180559945309417);"); break;
		case X86_INS_FLDL2E: e.line("fpush(c, 1.44269504088896340736);"); break;
		case X86_INS_FLDL2T: e.line("fpush(c, 3.32192809488736234787);"); break;
		case X86_INS_FLDLG2: e.line("fpush(c, 0.301029995663981195214);"); break;
		case X86_INS_FST: case X86_INS_FSTP:
		{
			bool pop = id == X86_INS_FSTP;
			if (isST(op(0))) e.line(fmt("ST(c, %d) = ST(c, 0);", sti(op(0))));
			else if (op(0).size == 4) e.line("stf32(" + e.ea(op(0)) + ", (float)ST(c, 0));");
			else if (op(0).size == 8) e.line("stf64(" + e.ea(op(0)) + ", ST(c, 0));");
			else { ok = false; break; }
			if (pop) e.line("fpop(c);");
			break;
		}
		case X86_INS_FIST: case X86_INS_FISTP: case X86_INS_FISTTP:
		{
			bool trunc = id == X86_INS_FISTTP;
			int bits = op(0).size * 8;
			std::string conv = trunc ? fmt("fcvt_mode(ST(c, 0), %d, 3)", bits) : fmt("fcvt(c, ST(c, 0), %d)", bits);
			if (bits == 64) e.line("guest::store<int64_t>(" + e.ea(op(0)) + ", " + conv + ");");
			else e.line(fmt("S%d(%s, (uint32_t)%s);", bits, e.ea(op(0)).c_str(), conv.c_str()));
			if (id != X86_INS_FIST) e.line("fpop(c);");
			break;
		}
		case X86_INS_FADD: case X86_INS_FIADD: case X86_INS_FSUB: case X86_INS_FSUBP: case X86_INS_FISUB:
		case X86_INS_FSUBR: case X86_INS_FSUBRP: case X86_INS_FISUBR: case X86_INS_FMUL: case X86_INS_FMULP: case X86_INS_FIMUL:
		case X86_INS_FDIV: case X86_INS_FDIVP: case X86_INS_FIDIV: case X86_INS_FDIVR: case X86_INS_FDIVRP: case X86_INS_FIDIVR:
		{
			int kind; bool rev, pop, isInt;
			switch (id)
			{
			case X86_INS_FADD: kind = 0; rev = 0; pop = in.mn == "faddp"; isInt = 0; break; case X86_INS_FIADD: kind = 0; rev = 0; pop = 0; isInt = 1; break;
			case X86_INS_FSUB: kind = 1; rev = 0; pop = 0; isInt = 0; break; case X86_INS_FSUBP: kind = 1; rev = 0; pop = 1; isInt = 0; break; case X86_INS_FISUB: kind = 1; rev = 0; pop = 0; isInt = 1; break;
			case X86_INS_FSUBR: kind = 1; rev = 1; pop = 0; isInt = 0; break; case X86_INS_FSUBRP: kind = 1; rev = 1; pop = 1; isInt = 0; break; case X86_INS_FISUBR: kind = 1; rev = 1; pop = 0; isInt = 1; break;
			case X86_INS_FMUL: kind = 2; rev = 0; pop = 0; isInt = 0; break; case X86_INS_FMULP: kind = 2; rev = 0; pop = 1; isInt = 0; break; case X86_INS_FIMUL: kind = 2; rev = 0; pop = 0; isInt = 1; break;
			case X86_INS_FDIV: kind = 3; rev = 0; pop = 0; isInt = 0; break; case X86_INS_FDIVP: kind = 3; rev = 0; pop = 1; isInt = 0; break; case X86_INS_FIDIV: kind = 3; rev = 0; pop = 0; isInt = 1; break;
			case X86_INS_FDIVR: kind = 3; rev = 1; pop = 0; isInt = 0; break; case X86_INS_FDIVRP: kind = 3; rev = 1; pop = 1; isInt = 0; break; default: kind = 3; rev = 1; pop = 0; isInt = 1; break;
			}
			std::string other; int dst = 0;
			if (op(0).type == X86_OP_MEM)
			{
				if (isInt) other = op(0).size == 2 ? "(double)(int16_t)G16(" + e.ea(op(0)) + ")" : "(double)(int32_t)G32(" + e.ea(op(0)) + ")";
				else other = op(0).size == 4 ? "(double)ldf32(" + e.ea(op(0)) + ")" : "ldf64(" + e.ea(op(0)) + ")";
			}
			else if (x.op_count == 2 && isST(op(0)) && isST(op(1))) { dst = sti(op(0)); other = fmt("ST(c, %d)", sti(op(1))); }
			else if (x.op_count == 1 && isST(op(0))) { if (pop) { dst = sti(op(0)); other = "ST(c, 0)"; } else { dst = 0; other = fmt("ST(c, %d)", sti(op(0))); } }
			else if (x.op_count == 0) { dst = 1; other = "ST(c, 0)"; pop = true; }   // fsubp etc. without operands
			else { ok = false; break; }
			e.line(fmt("farith(c, %d, %d, %s, %s, %s);", kind, dst, other.c_str(), rev ? "true" : "false", pop ? "true" : "false"));
			break;
		}
		case X86_INS_FXCH: e.line(fmt("{ double t = ST(c, 0); ST(c, 0) = ST(c, %d); ST(c, %d) = t; }", x.op_count ? sti(op(x.op_count - 1)) : 1, x.op_count ? sti(op(x.op_count - 1)) : 1)); break;
		case X86_INS_FCHS: e.line("ST(c, 0) = -ST(c, 0);"); break;
		case X86_INS_FABS: e.line("ST(c, 0) = fabs(ST(c, 0));"); break;
		case X86_INS_FSQRT: e.line("ST(c, 0) = fround(c, sqrt(ST(c, 0)));"); break;
		case X86_INS_FSIN: e.line("ST(c, 0) = sin(ST(c, 0));"); break;
		case X86_INS_FCOS: e.line("ST(c, 0) = cos(ST(c, 0));"); break;
		case X86_INS_FSINCOS: e.line("{ double v = ST(c, 0); ST(c, 0) = sin(v); fpush(c, cos(v)); }"); break;
		case X86_INS_FPTAN: e.line("ST(c, 0) = tan(ST(c, 0)); fpush(c, 1.0);"); break;
		case X86_INS_FPATAN: e.line("ST(c, 1) = atan2(ST(c, 1), ST(c, 0)); fpop(c);"); break;
		case X86_INS_FYL2X: e.line("ST(c, 1) = ST(c, 1) * log2(ST(c, 0)); fpop(c);"); break;
		case X86_INS_F2XM1: e.line("ST(c, 0) = exp2(ST(c, 0)) - 1.0;"); break;
		case X86_INS_FSCALE: e.line("ST(c, 0) = ldexp(ST(c, 0), (int)trunc(ST(c, 1)));"); break;
		case X86_INS_FRNDINT: e.line("ST(c, 0) = (double)fcvt(c, ST(c, 0), 64);"); break;
		case X86_INS_FCOM: case X86_INS_FCOMP: case X86_INS_FCOMPP: case X86_INS_FUCOM: case X86_INS_FUCOMP: case X86_INS_FUCOMPP:
		{
			std::string other = x.op_count == 0 ? "ST(c, 1)" : isST(op(0)) ? fmt("ST(c, %d)", sti(op(0)))
				: op(0).size == 4 ? "(double)ldf32(" + e.ea(op(0)) + ")" : "ldf64(" + e.ea(op(0)) + ")";
			e.line("fsetcc(c, ST(c, 0), " + other + ");");
			int pops = (id == X86_INS_FCOMPP || id == X86_INS_FUCOMPP) ? 2 : (id == X86_INS_FCOMP || id == X86_INS_FUCOMP) ? 1 : 0;
			for (int k = 0; k < pops; k++) e.line("fpop(c);");
			break;
		}
		case X86_INS_FCOMI: case X86_INS_FCOMPI: case X86_INS_FUCOMI: case X86_INS_FUCOMPI:
			e.line(fmt("{ unsigned fl = fcmpflags(ST(c, 0), ST(c, %d));", x.op_count ? sti(op(0)) : 1));
			e.flag(CF, "fl & 1"); e.flag(PF, "(fl >> 2) & 1"); e.flag(ZF, "(fl >> 6) & 1"); e.flag(OF, "0"); e.flag(SF, "0"); e.flag(AF, "0");
			e.line("}");
			if (id == X86_INS_FCOMPI || id == X86_INS_FUCOMPI) e.line("fpop(c);");
			break;
		case X86_INS_FNSTSW:
			if (op(0).type == X86_OP_REG) e.line("eax = (eax & 0xffff0000u) | fstatus(c);");
			else e.line("S16(" + e.ea(op(0)) + ", fstatus(c));");
			break;
		case X86_INS_FNSTCW: e.line("S16(" + e.ea(op(0)) + ", c->fcw);"); break;
		case X86_INS_FLDCW: e.line("c->fcw = G16(" + e.ea(op(0)) + ");"); break;
		case X86_INS_FNINIT: e.line("c->fcw = 0x37f; c->fcc = 0; c->top = 0;"); break;
		case X86_INS_FINCSTP: e.line("c->top = (c->top + 1) & 7;"); break;
		case X86_INS_FDECSTP: e.line("c->top = (c->top - 1) & 7;"); break;

		// ---- control flow
		case X86_INS_JMP:
		{
			auto& t = op(0);
			if (t.type == X86_OP_IMM) { e.line(jumpTo((uint32_t)t.imm)); break; }
			// indirect: switch over the known targets (jump tables), else dispatch as a tail call
			e.line("{ uint32_t t = " + e.rd(t, nullptr, 4) + ";");
			if (!in.succ.empty())
			{
				e.line("switch (t) {");
				std::set<uint32_t> seen;
				for (uint32_t s : in.succ)
					if (isLocal(s) && seen.insert(s).second) e.line(fmt("case 0x%08xu: goto L_%08x;", s, s));
				e.line("default: break; }");
			}
			e.line("SAVE(); call_target(c, t); return; }");
			break;
		}
		case X86_INS_CALL:
		{
			auto& t = op(0);
			if (t.type == X86_OP_IMM)
			{
				uint32_t target = (uint32_t)t.imm;
				if (target == in.next) { e.line(fmt("esp -= 4; S32(esp, %s);", hex(in.next).c_str())); break; }   // get-PC idiom
				if (setjmpThunks && setjmpThunks->count(target))
				{
					size_t k = sj.size();
					sj.push_back(in.next);
					e.line(fmt("esp -= 4; S32(esp, %s); SAVE(); f_%08x(c); LOAD();", hex(in.next).c_str(), target));
					e.line(fmt("SJ_%zu:; sj_esp[%zu] = esp;", k, k));
				}
				else if (funcs && !funcs->count(target))
					e.line(fmt("esp -= 4; S32(esp, %s); SAVE(); call_target(c, 0x%08xu); LOAD();", hex(in.next).c_str(), target));
				else
					e.line(fmt("esp -= 4; S32(esp, %s); SAVE(); f_%08x(c); LOAD();", hex(in.next).c_str(), target));
			}
			else if (setjmpSlot && t.type == X86_OP_MEM && t.mem.base == X86_REG_INVALID && t.mem.index == X86_REG_INVALID && (uint32_t)t.mem.disp == setjmpSlot)
			{
				// setjmp: a later longjmp throws GuestLongjmp; the catch in this function resumes right after the call
				size_t k = sj.size();
				sj.push_back(in.next);
				e.line("{ uint32_t t = " + e.rd(t, nullptr, 4) + "; esp -= 4; S32(esp, " + hex(in.next) + "); SAVE(); call_target(c, t); LOAD(); }");
				e.line(fmt("SJ_%zu:; sj_esp[%zu] = esp;", k, k));
			}
			else
			{
				e.line("{ uint32_t t = " + e.rd(t, nullptr, 4) + "; esp -= 4; S32(esp, " + hex(in.next) + "); SAVE(); call_target(c, t); LOAD(); }");
			}
			break;
		}
		case X86_INS_RET:
		{
			uint32_t pop = x.op_count ? (uint32_t)op(0).imm : 0;
			e.line("esp += " + std::to_string(4 + pop) + "; SAVE(); return;");
			break;
		}
		case X86_INS_INT3: case X86_INS_UD2: case X86_INS_HLT:
			e.line("unsupported(c, " + hex(in.va) + ", \"" + in.mn + "\");");
			break;

		default:
			if (isJcc(id) && x.op_count == 1 && op(0).type == X86_OP_IMM) { condJumpOrTail((uint32_t)op(0).imm); break; }
			ok = false;
		}

		if (!ok || e.failed)
		{
			unsup.push_back(fmt("%08x %s %s", in.va, in.mn.c_str(), in.ops.c_str()));
			e.o = "\tunsupported(c, " + hex(in.va) + ", \"" + in.mn + " " + in.ops + "\");\n";
		}
		if (!e.o.empty()) out += "\t{\n" + e.o + "\t}\n";
	}
};

}  // namespace

void init()
{
	cs_open(CS_ARCH_X86, CS_MODE_32, &cs);
	cs_option(cs, CS_OPT_DETAIL, CS_OPT_ON);
}

Result liftFunction(Reader& r, uint32_t entry, const Options& opt)
{
	gReader = &r;
	std::map<uint32_t, Ins> code;
	std::vector<uint32_t> work{entry};
	cs_insn* insn = cs_malloc(cs);
	auto isEntryOther = [&](uint32_t t) { return t != entry && opt.funcEntries && opt.funcEntries->count(t); };

	while (!work.empty())
	{
		uint32_t pc = work.back();
		work.pop_back();
		while (r.isCode(pc) && !code.count(pc))
		{
			uint8_t buf[16] = {};
			size_t avail = 0;
			for (; avail < 15 && r.read(pc + avail, buf + avail, 1); avail++) {}
			const uint8_t* p = buf;
			size_t left = avail;
			uint64_t a = pc;
			Ins in;
			in.va = pc;
			if (!cs_disasm_iter(cs, &p, &left, &a, insn))
			{
				// Undecodable bytes: record a trap so the rest of the function still lifts.
				in.id = 0; in.mn = "(bad)"; in.size = 1; in.next = pc + 1; in.terminal = true;
				code[pc] = in;
				break;
			}
			in.id = insn->id; in.mn = insn->mnemonic; in.ops = insn->op_str; in.size = insn->size; in.next = pc + insn->size;
			in.x = insn->detail->x86;
			bool jcc = isJcc(in.id) || in.id == X86_INS_LOOP || in.id == X86_INS_LOOPE || in.id == X86_INS_LOOPNE || in.id == X86_INS_JECXZ;
			if (in.id == X86_INS_RET || in.id == X86_INS_INT3 || in.id == X86_INS_UD2 || in.id == X86_INS_HLT) in.terminal = true;
			if (in.id == X86_INS_JMP)
			{
				in.terminal = true;
				auto& o = in.x.operands[0];
				if (o.type == X86_OP_IMM)
				{
					uint32_t t = (uint32_t)o.imm;
					if (!isEntryOther(t)) { in.succ.push_back(t); work.push_back(t); }
					else in.tailNeed = entryFlagNeed(t);
				}
				else if (o.type == X86_OP_MEM && o.mem.index != X86_REG_INVALID && o.mem.disp)
				{
					uint32_t tb = (uint32_t)o.mem.disp;
					for (uint32_t k = 0; k < 2048; k++)
					{
						uint32_t v;
						if (!r.read(tb + k * 4, (uint8_t*)&v, 4) || !r.isCode(v)) break;
						if (isEntryOther(v)) continue;   // a case that is another function's entry: reached via the default tail call
						in.succ.push_back(v);
						work.push_back(v);
					}
				}
			}
			if (jcc && in.x.op_count == 1 && in.x.operands[0].type == X86_OP_IMM)
			{
				uint32_t t = (uint32_t)in.x.operands[0].imm;
				if (!isEntryOther(t)) { in.succ.push_back(t); work.push_back(t); }
				else in.tailNeed = entryFlagNeed(t);
			}
			code[pc] = in;
			flagRW(code[pc]);
			if (in.terminal) break;
			pc = in.next;
		}
	}
	cs_free(insn, 1);

	// ---- flag liveness (backward dataflow over instructions)
	auto liveIn = [&](uint32_t va) -> unsigned {
		auto it = code.find(va);
		if (it == code.end()) return 0;
		Ins& i = it->second;
		return i.rd | (i.liveOut & ~i.wr);
	};
	for (bool changed = true; changed;)
	{
		changed = false;
		for (auto it = code.rbegin(); it != code.rend(); ++it)
		{
			Ins& i = it->second;
			unsigned out = 0;
			if (!i.terminal) out |= liveIn(i.next);
			for (uint32_t s : i.succ)
				if (code.count(s)) out |= liveIn(s);
			out |= i.tailNeed;
			if (i.id == X86_INS_CALL) out = 0;
			if (out != i.liveOut) { i.liveOut = out; changed = true; }
		}
	}

	// ---- emit
	std::set<uint32_t> labels;
	for (auto& [va, i] : code)
		for (uint32_t s : i.succ) labels.insert(s);

	Result res;
	Gen g{opt.funcEntries, entry, &code, {}};
	g.setjmpSlot = opt.setjmpSlot;
	g.setjmpThunks = &opt.setjmpThunks;
	std::string body;
	if (!code.empty() && code.begin()->first != entry)
	{
		// Some of the function's code lies below its entry (shared tails reached by jumps): start at the entry.
		labels.insert(entry);
		body += fmt("\tgoto L_%08x;\n", entry);
	}
	for (auto& [va, i] : code)
	{
		if (labels.count(va)) body += fmt("L_%08x:;\n", va);
		body += fmt("\t/* %08x %s %s */\n", va, i.mn.c_str(), i.ops.c_str());
		if (i.id == 0) { g.unsup.push_back(fmt("%08x (bad)", va)); body += "\tunsupported(c, " + hex(va) + ", \"undecodable\");\n"; }
		else g.gen(i, body);
		if (!i.terminal && !code.count(i.next))
			body += "\tunsupported(c, " + hex(va) + ", \"falls off the decoded function\");\n";
		res.instructions++;
	}

	static const std::set<uint32_t> hooks = [] {
		std::set<uint32_t> h;
		if (const char* e = getenv("RA3_HOOKS")) for (const char* p = e; *p;) { h.insert((uint32_t)strtoul(p, (char**)&p, 16)); if (*p == ',') p++; }
		return h;
	}();
	bool hooked = hooks.count(entry) != 0;
	std::string head = fmt("void f_%08x%s(CPU* c)\n{\n", entry, hooked ? "_real" : "");
	head += "\tuint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];\n";
	head += "\tuint8_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0, df = (uint8_t)c->df;\n";
	head += fmt("\tTRACE_FN(0x%08xu);\n", entry);
	head += "\t(void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of; (void)df;\n";
	if (entryFlagNeed(entry)) head += "\tLOADFL();\n";
	if (!g.sj.empty())
	{
		// every setjmp site can be re-entered after a longjmp: run the body in a retry loop
		std::string pre = fmt("\tuint32_t sj_esp[%zu] = {}; int sj_resume = 0;\n\tfor (;;) {\n\ttry {\n\tswitch (sj_resume) {\n", g.sj.size());
		for (size_t k = 0; k < g.sj.size(); k++) pre += fmt("\tcase %zu: goto SJ_%zu;\n", k + 1, k);
		pre += "\tdefault: break; }\n";
		std::string post = "\treturn;\n\t} catch (GuestLongjmp& ex) {\n";
		for (size_t k = 0; k < g.sj.size(); k++)
			post += fmt("\tif (sj_esp[%zu] == ex.esp && ex.eip == 0x%08xu) { ebp = G32(ex.buf); ebx = G32(ex.buf + 4); edi = G32(ex.buf + 8); esi = G32(ex.buf + 12); esp = ex.esp; eax = ex.val; sj_resume = %zu; continue; }\n", k, g.sj[k], k + 1);
		post += "\tthrow;\n\t}\n\t}\n";
		body = pre + body + post;
	}
	res.code = head + body + "}\n";
	if (hooked)
		res.code += fmt("void f_%08x(CPU* c)\n{\n\tuint32_t a0 = G32(c->r[4] + 4), a1 = G32(c->r[4] + 8), a2 = G32(c->r[4] + 12), ecx0 = c->r[1];\n\tf_%08x_real(c);\n\tra3_hook_after(c, 0x%08xu, a0, a1, a2, ecx0);\n}\n", entry, entry, entry);
	res.unsupported = g.unsup;
	return res;
}
}
