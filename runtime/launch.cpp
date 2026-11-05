// Shared start-up: maps the PE, creates the main guest thread and runs the entry point.
#include "launch.h"

#include <csignal>
#include <cstdio>
#include <unistd.h>

#include "pe_loader.h"

namespace rt
{
static void onFault(int sig, siginfo_t* si, void*)
{
	uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)guest::base;
	if (a >= b && a < b + (1ull << 32))
		log("FATAL: signal %d, guest memory access at %08x is not mapped", sig, (uint32_t)(a - b));
	else
		log("FATAL: signal %d at host address %p (runtime bug)", sig, si->si_addr);
	traceDump();
	_exit(72);
}

int launch(const std::string& gameFile)
{
	{
		struct sigaction sa{};
		sa.sa_sigaction = onFault;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGFPE, &sa, nullptr);
	}
	guest::init();
	heapInit();
	pe::Image img;
	std::string err;
	if (!pe::load(gameFile.c_str(), img, err)) fatal("PE load failed: %s", err.c_str());
	imageBase = img.base;
	imageEntry = img.base + img.entry;
	tls = {img.tlsStart, img.tlsEnd, img.tlsZeroFill, 0};
	log("loaded %s, entry %08x, %u imports", gameFile.c_str(), imageEntry, img.importCount);

	static CPU cpu;
	spawnMainThread(&cpu);
	callGuest(&cpu, imageEntry, {});
	log("entry returned, eax=%08x", cpu.r[EAX]);
	return (int)cpu.r[EAX];
}
}
