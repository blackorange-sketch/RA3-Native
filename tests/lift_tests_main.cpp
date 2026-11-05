// Lifts every 256-byte test snippet in snips.bin (base 0x1000000) and writes tests_gen.inc.
#include <cstdio>
#include <vector>

#include "../tools/lift.h"

struct Flat : lift::Reader
{
	std::vector<uint8_t> d;
	uint32_t base = 0x1000000;
	bool read(uint32_t va, uint8_t* o, size_t n) override
	{
		if (va < base || va + n > base + d.size()) return false;
		for (size_t i = 0; i < n; i++) o[i] = d[va - base + i];
		return true;
	}
	bool isCode(uint32_t va) override { return va >= base && va < base + d.size(); }
};

int main(int argc, char** argv)
{
	Flat f;
	FILE* in = fopen(argv[1], "rb");
	uint8_t b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof b, in)) > 0) f.d.insert(f.d.end(), b, b + n);
	fclose(in);
	lift::init();
	int count = (int)(f.d.size() / 256);
	FILE* out = fopen(argv[2], "w");
	fprintf(out, "#define NT %d\n", count);
	std::set<uint32_t> entries;
	for (int i = 0; i < count; i++) entries.insert(0x1000000 + i * 256);
	lift::Options opt;
	opt.funcEntries = &entries;
	int bad = 0;
	for (int i = 0; i < count; i++)
	{
		uint32_t e = 0x1000000 + i * 256;
		fprintf(out, "void f_%08x(CPU*);\n", e);
	}
	for (int i = 0; i < count; i++)
	{
		auto r = lift::liftFunction(f, 0x1000000 + i * 256, opt);
		fputs(r.code.c_str(), out);
		for (auto& u : r.unsupported) { fprintf(stderr, "test %d: unsupported %s\n", i, u.c_str()); bad++; }
	}
	fprintf(out, "static LiftedFn TESTFN[] = {\n");
	for (int i = 0; i < count; i++) fprintf(out, "\tf_%08x,\n", 0x1000000 + i * 256);
	fprintf(out, "};\n");
	fclose(out);
	return bad ? 4 : 0;
}
