// Lifts every function of a PE (list from analyze) and reports coverage. Writes C only with --emit.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "lift.h"

struct PE : lift::Reader
{
	struct S { uint32_t va, size, raw, flags; };
	std::vector<uint8_t> f;
	std::vector<S> s;
	uint32_t base = 0;
	const S* find(uint32_t va) const
	{
		for (auto& x : s) if (va >= base + x.va && va < base + x.va + x.size) return &x;
		return nullptr;
	}
	bool read(uint32_t va, uint8_t* o, size_t n) override
	{
		auto x = find(va);
		if (!x || va + n > base + x->va + x->size) return false;
		memcpy(o, &f[x->raw + (va - base - x->va)], n);
		return true;
	}
	bool isCode(uint32_t va) override { auto x = find(va); return x && (x->flags & 0x20000000); }
	template <class T> T rd(size_t o) { T v; memcpy(&v, &f[o], sizeof v); return v; }
	bool load(const char* p)
	{
		FILE* fp = fopen(p, "rb");
		if (!fp) return false;
		fseek(fp, 0, SEEK_END); f.resize(ftell(fp)); fseek(fp, 0, SEEK_SET);
		if (fread(f.data(), 1, f.size(), fp) != f.size()) return false;
		fclose(fp);
		uint32_t pe = rd<uint32_t>(0x3c);
		uint16_t n = rd<uint16_t>(pe + 6), os = rd<uint16_t>(pe + 20);
		base = rd<uint32_t>(pe + 24 + 28);
		for (int i = 0; i < n; i++)
		{
			size_t o = pe + 24 + os + i * 40;
			uint32_t vs = rd<uint32_t>(o + 8), rs = rd<uint32_t>(o + 16);
			s.push_back({rd<uint32_t>(o + 12), std::min(std::max(vs, rs), rs), rd<uint32_t>(o + 20), rd<uint32_t>(o + 36)});
		}
		return true;
	}
};

int main(int argc, char** argv)
{
	if (argc < 3) { fprintf(stderr, "usage: lift_pe <pe> <functions.txt> [--emit out.inc]\n"); return 1; }
	PE pe;
	if (!pe.load(argv[1])) return 1;
	std::set<uint32_t> funcs;
	FILE* fl = fopen(argv[2], "r");
	unsigned v;
	while (fscanf(fl, "%x", &v) == 1) funcs.insert(v);
	fclose(fl);
	FILE* out = argc > 4 && !strcmp(argv[3], "--emit") ? fopen(argv[4], "w") : nullptr;
	const char* dir = argc > 5 && !strcmp(argv[3], "--emit-dir") ? argv[4] : nullptr;
	int nfiles = dir ? atoi(argv[5]) : 0;
	std::vector<std::string> chunks;
	std::vector<uint32_t> order;
	lift::init();
	lift::Options opt;
	opt.funcEntries = &funcs;
	size_t full = 0, total = 0, ins = 0, bad = 0;
	std::map<std::string, size_t> byMn, funcsByMn;
	for (uint32_t e : funcs)
	{
		auto r = lift::liftFunction(pe, e, opt);
		total++; ins += r.instructions; bad += r.unsupported.size();
		if (r.unsupported.empty()) full++;
		std::set<std::string> seen;
		for (auto& u : r.unsupported)
		{
			std::string mn = u.substr(9, u.find(' ', 9) - 9);
			byMn[mn]++;
			if (seen.insert(mn).second) funcsByMn[mn]++;
		}
		if (out) fputs(r.code.c_str(), out);
		if (dir) { chunks.push_back(r.code); order.push_back(e); }
	}
	if (dir)
	{
		std::string d = dir;
		FILE* h = fopen((d + "/lifted_decl.h").c_str(), "w");
		fprintf(h, "#pragma once\n#include \"lifted_prelude.h\"\n");
		for (uint32_t e : order) fprintf(h, "void f_%08x(CPU*);\n", e);
		fclose(h);
		for (int i = 0; i < nfiles; i++)
		{
			FILE* f = fopen((d + "/lifted_" + std::to_string(i) + ".cpp").c_str(), "w");
			fprintf(f, "#include \"lifted_decl.h\"\n#include <cmath>\n");
			for (size_t k = i; k < chunks.size(); k += nfiles) fputs(chunks[k].c_str(), f);
			fclose(f);
		}
		FILE* t = fopen((d + "/lifted_table.cpp").c_str(), "w");
		fprintf(t, "#include \"lifted_decl.h\"\nstruct LiftedEntry { uint32_t addr; LiftedFn fn; };\nextern const LiftedEntry lifted_table[] = {\n");
		for (uint32_t e : order) fprintf(t, "\t{0x%08xu, f_%08x},\n", e, e);
		fprintf(t, "};\nextern const unsigned lifted_count = %zu;\n", order.size());
		fclose(t);
	}
	printf("functions %zu, fully lifted %zu (%.1f%%), instructions %zu, unsupported %zu (%.2f%%)\n", total, full, 100.0 * full / total, ins, bad, 100.0 * bad / ins);
	std::vector<std::pair<size_t, std::string>> v2;
	for (auto& [k, n] : byMn) v2.push_back({n, k});
	std::sort(v2.rbegin(), v2.rend());
	printf("unsupported by mnemonic (instances / functions affected):\n");
	for (size_t i = 0; i < v2.size() && i < 200; i++) printf("  %-12s %8zu %8zu\n", v2[i].second.c_str(), v2[i].first, funcsByMn[v2[i].second]);
	return 0;
}
