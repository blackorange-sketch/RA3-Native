// Shared start-up: maps the PE, creates the main guest thread and runs the entry point.
#include <chrono>
#include <thread>
#include "launch.h"

#include <algorithm>
#include <vector>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <cstdio>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>
#include "pe_loader.h"

void dumpWaiters();
namespace rt
{
static void emit(const char* s)
{
	if (!config.logFile.empty())
		if (int fd = open(config.logFile.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666); fd >= 0) { write(fd, s, strlen(s)); close(fd); }
	if (logHook) logHook(s);
	fputs(s, stderr);
}

static void onFault(int sig, siginfo_t* si, void*)
{
	// Deliberately lock-free: the fault may have happened inside log().
	char buf[256];
	uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)guest::base;
	if (a >= b && a < b + (1ull << 32))
		snprintf(buf, sizeof buf, "FATAL: signal %d (code %d), guest memory access at %08x", sig, si->si_code, (uint32_t)(a - b));
	else
		snprintf(buf, sizeof buf, "FATAL: signal %d (code %d) at host address %p (stack overflow or runtime bug)", sig, si->si_code, si->si_addr);
	emit(buf);
	emit("\n");
	traceDump();
	_exit(72);
}

static void installAltStack()
{
	static thread_local bool done = false;
	if (done) return;
	done = true;
	stack_t ss{};
	ss.ss_size = 64 * 1024;
	ss.ss_sp = malloc(ss.ss_size);
	sigaltstack(&ss, nullptr);
}

void spawnHostThread(std::function<void()> fn, size_t stackBytes)
{
	auto* f = new std::function<void()>([fn] { installAltStack(); fn(); });
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stackBytes);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t th;
	if (pthread_create(&th, &attr, [](void* p) -> void* { auto* fn = static_cast<std::function<void()>*>(p); (*fn)(); delete fn; return nullptr; }, f) != 0)
		fatal("cannot create a host thread");
	pthread_attr_destroy(&attr);
}

static void listDir(const std::string& d, int depth)
{
	DIR* dir = opendir(d.c_str());
	if (!dir) return;
	std::vector<std::string> names;
	while (auto e = readdir(dir)) if (e->d_name[0] != '.') names.push_back(e->d_name);
	closedir(dir);
	std::sort(names.begin(), names.end());
	int shown = 0;
	for (auto& n : names)
	{
		struct stat st;
		bool isDir = stat((d + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
		if (shown++ < 40) log("  %s%s%s", std::string(2 * (3 - depth), ' ').c_str(), n.c_str(), isDir ? "/" : "");
		if (isDir && depth > 1) listDir(d + "/" + n, depth - 1);
	}
	if ((int)names.size() > 40) log("  ... %d entries total", (int)names.size());
}
int launch(const std::string& gameFile)
{
	log("game root listing: %s", config.gameRoot.c_str());
	listDir(config.gameRoot, 2);
	{
		struct sigaction sa{};
		sa.sa_sigaction = onFault;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigaction(SIGSEGV, &sa, nullptr);
		sigaction(SIGBUS, &sa, nullptr);
		sigaction(SIGFPE, &sa, nullptr);
	}
	installAltStack();
	// Progress watchdog: every 20 s dump what every thread has been doing (hang vs. busy diagnosis).
	std::thread([] {
		for (int i = 1; i <= 15; i++)
		{
			std::this_thread::sleep_for(std::chrono::seconds(20));
			log("watchdog: %d s since start", i * 20);
			dumpWaiters();
			traceDump();
		}
	}).detach();
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
