// Shared start-up: maps the PE, creates the main guest thread and runs the entry point.
#include <dlfcn.h>
#include <ucontext.h>
#include <chrono>
#include <thread>
#include "launch.h"

#include <algorithm>
#include <vector>
#include <csignal>
#include <atomic>
#include <sys/mman.h>
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

static std::atomic<uint32_t> gWatchPage{0};
static std::atomic<int> gWatchHits{0};
void watchArm(uint32_t ga)
{
	uint32_t pg = ga & ~0xfffu;
	gWatchPage = pg;
	mprotect(guest::base + pg, 4096, PROT_READ);
}
bool watchHit(uint32_t ga)
{
	uint32_t pg = gWatchPage.load();
	if (!pg || (ga & ~0xfffu) != pg) return false;
	mprotect(guest::base + pg, 4096, PROT_READ | PROT_WRITE);
	gWatchPage = 0;
	int n = gWatchHits++;
	if (n < 80)
	{
		char buf[128];
		snprintf(buf, sizeof buf, "WATCH write #%d at guest %08x (page %08x)\n", n, ga, pg);
		emit(buf);
		traceDumpCurrent(32);
	}
	return true;
}

static void onFault(int sig, siginfo_t* si, void* uctx)
{
	{
		uintptr_t a0 = (uintptr_t)si->si_addr, b0 = (uintptr_t)guest::base;
		if (a0 >= b0 && a0 < b0 + (1ull << 32) && watchHit((uint32_t)(a0 - b0))) return;
	}
	// Deliberately lock-free: the fault may have happened inside log().
	char buf[256];
	uintptr_t a = (uintptr_t)si->si_addr, b = (uintptr_t)guest::base;
	if (a >= b && a < b + (1ull << 32))
		snprintf(buf, sizeof buf, "FATAL: signal %d (code %d), guest memory access at %08x", sig, si->si_code, (uint32_t)(a - b));
	else
		snprintf(buf, sizeof buf, "FATAL: signal %d (code %d) at host address %p (stack overflow or runtime bug)", sig, si->si_code, si->si_addr);
	emit(buf);
	emit("\n");
	{ char t[64]; snprintf(t, sizeof t, "FAULTING host tid %d\n", (int)gettid()); emit(t); }
#if defined(__aarch64__)
	if (uctx)
	{
		// where in host code: pc / lr and a short frame-pointer walk, as module+offset (and symbol when exported)
		ucontext_t* uc = (ucontext_t*)uctx;
		uintptr_t pcs[12]; int n = 0;
		pcs[n++] = (uintptr_t)uc->uc_mcontext.pc;
		pcs[n++] = (uintptr_t)uc->uc_mcontext.regs[30];
		uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
		for (int i = 0; i < 10 && fp && !(fp & 7); i++)
		{
			uintptr_t next = ((uintptr_t*)fp)[0], ret = ((uintptr_t*)fp)[1];
			if (!ret) break;
			pcs[n++] = ret;
			if (next <= fp || next - fp > (1u << 20)) break;
			fp = next;
		}
		for (int i = 0; i < n; i++)
		{
			Dl_info di{};
			char t[320];
			if (pcs[i] && dladdr((void*)pcs[i], &di) && di.dli_fname)
			{
				const char* base = strrchr(di.dli_fname, '/');
				snprintf(t, sizeof t, "   host #%d %s+0x%zx %s+0x%zx\n", i, base ? base + 1 : di.dli_fname, (size_t)(pcs[i] - (uintptr_t)di.dli_fbase),
					di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? (size_t)(pcs[i] - (uintptr_t)di.dli_saddr) : 0);
			}
			else snprintf(t, sizeof t, "   host #%d %p\n", i, (void*)pcs[i]);
			emit(t);
		}
	}
#endif
	if (CPU* gc = thisThread().cpu)
	{
		// guest registers (as of the last call boundary) and the top of the guest stack, with printable text
		char t[400];
		snprintf(t, sizeof t, "   guest eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", gc->r[EAX], gc->r[ECX], gc->r[EDX], gc->r[EBX], gc->r[ESP], gc->r[EBP], gc->r[ESI], gc->r[EDI]);
		emit(t);
		uint32_t sp = gc->r[ESP] & ~3u;
		if (sp > 0x60000000u && sp < 0x7fff0000u)
			for (uint32_t row = 0; row < 0x280; row += 32)
			{
				int n = snprintf(t, sizeof t, "   stk %08x:", sp + row);
				for (uint32_t k = 0; k < 32; k += 4) n += snprintf(t + n, sizeof t - n, " %08x", guest::load<uint32_t>(sp + row + k));
				n += snprintf(t + n, sizeof t - n, "  ");
				for (uint32_t k = 0; k < 32; k++) { uint8_t ch = guest::load<uint8_t>(sp + row + k); t[n++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.'; }
				t[n++] = '\n'; t[n] = 0;
				emit(t);
			}
	}
	traceDumpCurrent(64);
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
	if (verbose()) std::thread([] {
		for (int i = 1; i <= 15; i++)
		{
			std::this_thread::sleep_for(std::chrono::seconds(20));
			log("watchdog: %d s since start", i * 20);
			dumpWaiters();
			if (i <= 2) traceDump();
			callStats();
			traceProfile(3000);
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
