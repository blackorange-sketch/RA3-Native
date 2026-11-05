// PC runner: loads the game PE, sets up the main guest thread and calls the PE entry point.
#include <cstdio>

#include "pe_loader.h"
#include "rt.h"

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: ra3run <ra3_1.12.game> [gameRoot] [dataRoot]\n"); return 2; }
	if (argc > 2) rt::config.gameRoot = argv[2];
	if (argc > 3) rt::config.dataRoot = argv[3];
	if (const char* lf = getenv("RA3_LOG")) rt::config.logFile = lf;

	guest::init();
	rt::heapInit();
	pe::Image img;
	std::string err;
	if (!pe::load(argv[1], img, err)) rt::fatal("PE load failed: %s", err.c_str());
	rt::imageBase = img.base;
	rt::imageEntry = img.base + img.entry;
	rt::tls = {img.tlsStart, img.tlsEnd, img.tlsZeroFill, 0};
	rt::log("loaded %s, entry %08x, %u imports", argv[1], rt::imageEntry, img.importCount);

	static CPU cpu;
	rt::spawnMainThread(&cpu);
	rt::callGuest(&cpu, rt::imageEntry, {});
	rt::log("entry returned, eax=%08x", cpu.r[EAX]);
	return (int)cpu.r[EAX];
}
