// analyze: static survey of a 32-bit PE (the RA3 game binary) for recompilation planning.
// Finds functions by recursive descent, counts hard cases (jump tables, indirect calls, privileged
// or exotic instructions) and writes a function list. Reads the user's own binary; writes no game data.
#include <capstone/capstone.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Section { std::string name; uint32_t va, vsize, raw, rsize, flags; };

static std::vector<uint8_t> file;
static std::vector<Section> secs;
static uint32_t imageBase, entryRva;
static std::map<uint32_t, std::string> iat;  // VA of IAT slot -> "dll!name"

template <class T> static T rd(size_t off) { T v; memcpy(&v, &file[off], sizeof v); return v; }

static const Section* secOfRva(uint32_t rva)
{
	for (auto& s : secs)
		if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rsize))
			return &s;
	return nullptr;
}

static bool readVA(uint32_t va, uint8_t* out, size_t n)
{
	uint32_t rva = va - imageBase;
	auto s = secOfRva(rva);
	if (!s) return false;
	uint32_t off = rva - s->va;
	if (off + n > s->rsize) return false;
	memcpy(out, &file[s->raw + off], n);
	return true;
}

static bool isCode(uint32_t va)
{
	auto s = secOfRva(va - imageBase);
	return s && (s->flags & 0x20000000);
}

static void loadPE(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	file.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(file.data(), 1, file.size(), f) != file.size()) exit(1);
	fclose(f);

	uint32_t pe = rd<uint32_t>(0x3c);
	uint16_t n = rd<uint16_t>(pe + 6), optSize = rd<uint16_t>(pe + 20);
	size_t opt = pe + 24;
	entryRva = rd<uint32_t>(opt + 16);
	imageBase = rd<uint32_t>(opt + 28);
	size_t sec = opt + optSize;
	for (int i = 0; i < n; i++)
	{
		size_t o = sec + i * 40;
		Section s;
		s.name.assign((char*)&file[o], strnlen((char*)&file[o], 8));
		s.vsize = rd<uint32_t>(o + 8); s.va = rd<uint32_t>(o + 12);
		s.rsize = rd<uint32_t>(o + 16); s.raw = rd<uint32_t>(o + 20);
		s.flags = rd<uint32_t>(o + 36);
		secs.push_back(s);
	}

	// Import directory
	uint32_t impRva = rd<uint32_t>(opt + 96 + 8);
	auto rvaOff = [&](uint32_t rva) -> size_t {
		auto s = secOfRva(rva);
		return s ? s->raw + (rva - s->va) : (size_t)-1;
	};
	for (size_t d = rvaOff(impRva);; d += 20)
	{
		uint32_t nameRva = rd<uint32_t>(d + 12), first = rd<uint32_t>(d + 16), oft = rd<uint32_t>(d);
		if (!nameRva) break;
		std::string dll = (char*)&file[rvaOff(nameRva)];
		uint32_t thunk = oft ? oft : first;
		for (int i = 0;; i++)
		{
			uint32_t t = rd<uint32_t>(rvaOff(thunk) + i * 4);
			if (!t) break;
			std::string nm = (t & 0x80000000) ? "#" + std::to_string(t & 0xffff) : (char*)&file[rvaOff(t) + 2];
			iat[imageBase + first + i * 4] = dll + "!" + nm;
		}
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: analyze <pe> <out-prefix>\n"); return 1; }
	loadPE(argv[1]);
	std::string prefix = argv[2];

	csh h;
	cs_open(CS_ARCH_X86, CS_MODE_32, &h);
	cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
	cs_insn* insn = cs_malloc(h);

	std::set<uint32_t> funcs, seenInsn;
	std::vector<uint32_t> work;
	auto addFunc = [&](uint32_t va) {
		if (isCode(va) && funcs.insert(va).second) work.push_back(va);
	};

	addFunc(imageBase + entryRva);

	// Seeds: aligned pointers into code in data sections (vtables, callback tables).
	size_t dataPtrs = 0;
	for (auto& s : secs)
	{
		if (s.flags & 0x20000000) continue;
		for (uint32_t o = 0; o + 4 <= s.rsize; o += 4)
		{
			uint32_t v = rd<uint32_t>(s.raw + o);
			if (v >= imageBase && isCode(v)) { addFunc(v); dataPtrs++; }
		}
	}

	std::map<std::string, uint64_t> st;
	std::map<std::string, uint64_t> imports;
	std::set<uint32_t> tableTargets;
	std::map<std::string, uint64_t> exotic;

	while (!work.empty())
	{
		uint32_t pc = work.back();
		work.pop_back();
		std::vector<uint32_t> local{pc};
		while (!local.empty())
		{
			pc = local.back();
			local.pop_back();
			while (isCode(pc) && seenInsn.insert(pc).second)
			{
				uint8_t buf[16];
				size_t avail = 0;
				for (; avail < 15 && readVA(pc + avail, buf + avail, 1); avail++) {}
				const uint8_t* p = buf;
				size_t sz = avail;
				uint64_t a = pc;
				if (!cs_disasm_iter(h, &p, &sz, &a, insn)) { st["undecodable"]++; break; }
				st["instructions"]++;
				cs_x86& x = insn->detail->x86;
				auto id = insn->id;
				bool term = false;

				for (int g = 0; g < insn->detail->groups_count; g++)
				{
					switch (insn->detail->groups[g])
					{
					case X86_GRP_MMX: st["grp_mmx"]++; break;
					case X86_GRP_SSE1: st["grp_sse1"]++; break;
					case X86_GRP_SSE2: st["grp_sse2"]++; break;
					case X86_GRP_SSE3: case X86_GRP_SSSE3: case X86_GRP_SSE41: case X86_GRP_SSE42: st["grp_sse3+"]++; break;
					case X86_GRP_FPU: st["grp_x87"]++; break;
					case X86_GRP_PRIVILEGE: case X86_GRP_INT: st["grp_priv_int"]++; exotic[insn->mnemonic]++; break;
					}
				}
				if (id == X86_INS_CPUID || id == X86_INS_RDTSC || id == X86_INS_IN || id == X86_INS_OUT ||
					id == X86_INS_SYSENTER || id == X86_INS_XGETBV)
					exotic[insn->mnemonic]++;
				for (int i = 0; i < x.op_count; i++)
					if (x.operands[i].type == X86_OP_MEM && x.operands[i].mem.segment == X86_REG_FS)
					{ st["fs_access"]++; break; }

				// Code addresses materialized as immediates (callbacks, `mov eax, fn; call eax`).
				if (id != X86_INS_CALL && id != X86_INS_JMP)
					for (int i = 0; i < x.op_count; i++)
						if (x.operands[i].type == X86_OP_IMM)
						{
							uint32_t v = (uint32_t)x.operands[i].imm;
							uint8_t prev = 0;
							if (v >= imageBase && isCode(v) && readVA(v - 1, &prev, 1) && (prev == 0xcc || prev == 0xc3 || prev == 0x90 || prev == 0xc2) && !funcs.count(v))
							{ addFunc(v); st["imm_code_seed"]++; }
						}
				if (id == X86_INS_CALL || id == X86_INS_JMP || (insn->detail->groups_count && false)) {}
				bool isJcc = false, isCall = id == X86_INS_CALL, isJmp = id == X86_INS_JMP;
				for (int g = 0; g < insn->detail->groups_count; g++)
					if (insn->detail->groups[g] == X86_GRP_JUMP && !isJmp) isJcc = true;

				if (isCall || isJmp || isJcc)
				{
					auto& o = x.operands[0];
					if (o.type == X86_OP_IMM)
					{
						uint32_t t = (uint32_t)o.imm;
						if (isCall) { st["call_direct"]++; addFunc(t); }
						else if (isJmp && isCode(t)) { // tail call or in-function jump: follow as code
							local.push_back(t);
						}
						else if (isJcc) local.push_back(t);
					}
					else if (o.type == X86_OP_MEM)
					{
						bool abs = o.mem.base == X86_REG_INVALID && o.mem.index == X86_REG_INVALID;
						if (abs && iat.count((uint32_t)o.mem.disp))
						{
							imports[iat[(uint32_t)o.mem.disp]]++;
							st[isCall ? "call_import" : "jmp_import"]++;
						}
						else if (isJmp && o.mem.index != X86_REG_INVALID && o.mem.disp)
						{
							st["jump_table"]++;
							uint32_t tb = (uint32_t)o.mem.disp;
							for (uint32_t k = 0; k < 4096; k++)
							{
								uint32_t v;
								if (!readVA(tb + k * 4, (uint8_t*)&v, 4) || !isCode(v)) break;
								tableTargets.insert(v);
								local.push_back(v);
							}
						}
						else st[isCall ? "call_indirect_mem" : "jmp_indirect_mem"]++;
					}
					else st[isCall ? "call_indirect_reg" : "jmp_indirect_reg"]++;
					if (isJmp) term = true;
				}
				if (id == X86_INS_RET || id == X86_INS_INT3 || id == X86_INS_UD2 || id == X86_INS_HLT) term = true;
				if (term) break;
				pc += insn->size;
			}
		}
	}

	printf("image base 0x%x entry 0x%x\n", imageBase, imageBase + entryRva);
	printf("functions: %zu (seeds from data pointers: %zu)\n", funcs.size(), dataPtrs);
	printf("distinct jump-table targets: %zu\n", tableTargets.size());
	for (auto& [k, v] : st) printf("%-20s %llu\n", k.c_str(), (unsigned long long)v);
	printf("exotic instructions:\n");
	for (auto& [k, v] : exotic) printf("  %-10s %llu\n", k.c_str(), (unsigned long long)v);

	FILE* o = fopen((prefix + ".functions.txt").c_str(), "w");
	for (auto v : funcs) fprintf(o, "%08x\n", v);
	fclose(o);
	o = fopen((prefix + ".imports_used.txt").c_str(), "w");
	std::vector<std::pair<uint64_t, std::string>> iv;
	for (auto& [k, v] : imports) iv.push_back({v, k});
	std::sort(iv.rbegin(), iv.rend());
	for (auto& [v, k] : iv) fprintf(o, "%8llu %s\n", (unsigned long long)v, k.c_str());
	fclose(o);

	// Code coverage of .text
	for (auto& s : secs)
		if (s.flags & 0x20000000)
		{
			uint64_t covered = 0;
			for (uint32_t va = imageBase + s.va; va < imageBase + s.va + s.rsize; va++)
				if (seenInsn.count(va)) covered++;
			printf("%s: %u bytes, instruction starts found %llu\n", s.name.c_str(), s.rsize, (unsigned long long)covered);
		}
	return 0;
}
