// Maps a PE into the guest window and reports what the loader did. Used to check the loader on a PC.
#include <cstdio>
#include <map>

#include "guest.h"
#include "imports.h"
#include "pe_loader.h"

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: loadtest <pe>\n"); return 1; }
	pe::Image img;
	std::string err;
	if (!pe::load(argv[1], img, err)) { fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }

	printf("base 0x%08x entry 0x%08x size 0x%x\n", img.base, img.base + img.entry, img.sizeOfImage);
	for (auto& s : img.sections)
		printf("  %-8s va 0x%08x size 0x%08x flags 0x%08x\n", s.name.c_str(), img.base + s.va, s.vsize, s.flags);
	printf("tls: data 0x%x..0x%x index@0x%x callbacks@0x%x\n", img.tlsStart, img.tlsEnd, img.tlsIndexAddr, img.tlsCallbacks);

	std::map<std::string, int> perDll;
	for (auto& e : imports::all()) perDll[e.dll]++;
	printf("imports bound: %u in %zu DLLs\n", img.importCount, perDll.size());

	// Spot-check: entry bytes and an IAT slot resolve through the stub table.
	uint32_t ep = img.base + img.entry;
	printf("entry bytes:");
	for (int i = 0; i < 8; i++) printf(" %02x", guest::load<uint8_t>(ep + i));
	printf("\n");
	const auto& all = imports::all();
	if (!all.empty())
	{
		auto* e = imports::lookup(all.front().stub);
		printf("first import: %s!%s -> stub 0x%08x (lookup %s)\n", e->dll.c_str(), e->name.c_str(), e->stub, e ? "ok" : "FAIL");
	}

	return 0;
}
