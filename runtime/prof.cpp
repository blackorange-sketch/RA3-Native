// Sampling profiler for the game's main thread (enabled by /sdcard/ra3_prof).
// A per-thread CPU-time timer sends SIGPROF; the handler records the interrupted host pc. A background thread
// resolves the samples to symbols (lifted functions are exported as f_<guest address>) every 30 s and logs the
// hottest functions.
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <ucontext.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <sys/syscall.h>
#include <pthread.h>
#include <chrono>

#include "rt.h"

namespace prof
{
namespace
{
constexpr uint32_t N = 1u << 17;
uintptr_t samples[N];
std::atomic<uint32_t> head{0};

void onProf(int, siginfo_t*, void* uctx)
{
#if defined(__aarch64__)
	ucontext_t* uc = (ucontext_t*)uctx;
	uint32_t i = head.fetch_add(1, std::memory_order_relaxed);
	samples[i & (N - 1)] = (uintptr_t)uc->uc_mcontext.pc;
#else
	(void)uctx;
#endif
}

std::string nameOf(uintptr_t pc, std::unordered_map<uintptr_t, std::string>& cache)
{
	auto it = cache.find(pc);
	if (it != cache.end()) return it->second;
	Dl_info di{};
	std::string n;
	if (dladdr((void*)pc, &di) && di.dli_fname)
	{
		const char* base = strrchr(di.dli_fname, '/');
		base = base ? base + 1 : di.dli_fname;
		if (di.dli_sname) n = di.dli_sname;
		else { char b[96]; snprintf(b, sizeof b, "%s+0x%zx", base, (size_t)(pc - (uintptr_t)di.dli_fbase) & ~(size_t)0xfff); n = b; }
		if (strcmp(base, "libra3native.so") != 0) n = std::string(base) + ":" + n;
	}
	else n = "?";
	cache[pc] = n;
	return n;
}

void reporter()
{
	std::unordered_map<uintptr_t, std::string> cache;
	uint32_t tail = 0;
	for (int round = 0;; round++)
	{
		std::this_thread::sleep_for(std::chrono::seconds(30));
		uint32_t h = head.load();
		uint32_t n = h - tail;
		if (n > N) { tail = h - N; n = N; }
		std::unordered_map<uintptr_t, uint32_t> perPc;
		for (uint32_t i = tail; i != h; i++) perPc[samples[i & (N - 1)]]++;
		tail = h;
		if (!n) continue;
		std::unordered_map<std::string, uint32_t> perFn;
		for (auto& kv : perPc) perFn[nameOf(kv.first, cache)] += kv.second;
		std::vector<std::pair<uint32_t, std::string>> v;
		for (auto& kv : perFn) v.push_back({kv.second, kv.first});
		std::sort(v.rbegin(), v.rend());
		uint32_t lifted = 0;
		for (auto& kv : perFn) if (kv.first.compare(0, 2, "f_") == 0) lifted += kv.second;
		rt::log("prof: %u samples (main thread CPU time), %.1f%% in lifted game code, %zu functions; top:", n, 100.0 * lifted / n, v.size());
		std::string line;
		for (size_t i = 0; i < v.size() && i < 40; i++)
		{
			char b[160]; snprintf(b, sizeof b, " %s %.1f%% |", v[i].second.c_str(), 100.0 * v[i].first / n);
			line += b;
			if ((i + 1) % 8 == 0) { rt::log("prof:%s", line.c_str()); line.clear(); }
		}
		if (!line.empty()) rt::log("prof:%s", line.c_str());
	}
}
}

void startForThisThread()
{
	if (access("/sdcard/ra3_prof", F_OK) != 0) return;
	struct sigaction sa{};
	sa.sa_sigaction = onProf;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, nullptr);
	sigevent ev{};
	ev.sigev_notify = SIGEV_THREAD_ID;
	ev.sigev_signo = SIGPROF;
#ifdef sigev_notify_thread_id
	ev.sigev_notify_thread_id = (pid_t)syscall(SYS_gettid);
#else
	ev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
#endif
	clockid_t clk;
	timer_t t;
	// CPU time of this thread: samples only while it runs (waits are visible in the perf: line instead)
	if (pthread_getcpuclockid(pthread_self(), &clk) != 0) clk = CLOCK_MONOTONIC;
	if (timer_create(clk, &ev, &t) != 0) { rt::log("prof: timer_create failed"); return; }
	itimerspec its{};
	its.it_interval.tv_nsec = 1000000;   // 1 ms
	its.it_value.tv_nsec = 1000000;
	timer_settime(t, 0, &its, nullptr);
	std::thread(reporter).detach();
	rt::log("prof: sampling the main thread every 1 ms of CPU time");
}
}

void profStart() { prof::startForThisThread(); }
