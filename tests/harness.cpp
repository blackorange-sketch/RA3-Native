// 64-bit side of the differential test: runs lifted code with the same inputs as native_runner.asm.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../runtime/cpu.h"
#include "../runtime/lifted_prelude.h"

void call_target(CPU*, uint32_t a) { fprintf(stderr, "call_target 0x%x\n", a); exit(3); }
void unsupported(CPU*, uint32_t va, const char* w) { fprintf(stderr, "unsupported at %08x: %s\n", va, w); exit(2); }

#include "tests_gen.inc"

static uint32_t seed = 0x2545F491;
static uint32_t rnd() { uint32_t x = seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return seed = x; }
static uint32_t biased()
{
	uint32_t v = rnd();
	switch (v & 7)
	{
	case 0: return 0; case 1: return 1; case 2: return 0xffffffffu; case 3: return 0x80000000u;
	case 4: return 0x7fffffffu; case 5: return v & 0xff; default: return v;
	}
}

int main()
{
	guest::init();
	guest::commit(0x2000000, 0x2000);
	for (int t = 0; t < NT; t++)
		for (int it = 0; it < 48; it++)
		{
			for (int i = 0; i < 1024; i++) guest::store<uint32_t>(0x2000000 + i * 4, rnd());
			uint32_t regs[8] = {};
			for (int i = 0; i < 8; i++) if (i != 4) regs[i] = biased();
			CPU c{};
			for (int i = 0; i < 8; i++) c.r[i] = regs[i];
			c.r[4] = 0x2001efc;   // as if `call` had pushed the return address
			guest::store<uint32_t>(0x2001efc, 0);
			TESTFN[t](&c);
			uint32_t rec[11];
			rec[0] = t; rec[1] = it;
			for (int i = 0; i < 8; i++) rec[2 + i] = c.r[i];
			uint32_t h = 0;
			for (int i = 0; i < 1024; i++) { h = (h << 5) | (h >> 27); h ^= guest::load<uint32_t>(0x2000000 + i * 4); }
			rec[10] = h;
			fwrite(rec, 4, 11, stdout);
		}
	return 0;
}
