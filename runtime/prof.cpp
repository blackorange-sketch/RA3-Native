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
#include <dirent.h>

#include "rt.h"

namespace prof
{
namespace
{
constexpr uint32_t N = 1u << 17;
uintptr_t samples[N], callers[N];
constexpr int Deep = 4;
uintptr_t deep[N][Deep];          // return addresses from the frame-pointer chain (who called the libc caller)
uintptr_t stackLo = 0, stackHi = 0;
std::atomic<uint32_t> head{0};

void onProf(int, siginfo_t*, void* uctx)
{
#if defined(__aarch64__)
	ucontext_t* uc = (ucontext_t*)uctx;
	uint32_t i = head.fetch_add(1, std::memory_order_relaxed);
	samples[i & (N - 1)] = (uintptr_t)uc->uc_mcontext.pc;
	callers[i & (N - 1)] = (uintptr_t)uc->uc_mcontext.regs[30];
	// frame records {prev fp, return address}; only followed while they stay inside this thread's stack
	uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
	for (int k = 0; k < Deep; k++)
	{
		uintptr_t ra = 0;
		if (fp >= stackLo && fp + 16 <= stackHi && !(fp & 7))
		{
			ra = ((uintptr_t*)fp)[1];
			uintptr_t next = ((uintptr_t*)fp)[0];
			fp = next > fp ? next : 0;
		}
		else fp = 0;
		deep[i & (N - 1)][k] = ra;
	}
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
		if (di.dli_sname)
		{
			n = di.dli_sname;
			size_t f = n.find("f_00");
			if (n.compare(0, 2, "_Z") == 0 && f != std::string::npos && f < 6) n = n.substr(f, 10);
		}
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
		std::unordered_map<std::string, uint32_t> perCaller;   // for samples in code without a symbol (helpers, libc): who called it
		for (uint32_t i = tail; i != h; i++) perPc[samples[i & (N - 1)]]++;
		for (uint32_t i = tail; i != h; i++)
		{
			const std::string& nm = nameOf(samples[i & (N - 1)], cache);
			if (nm.find('+') != std::string::npos || nm.find(':') != std::string::npos) perCaller[nm.substr(0, 40) + " <- " + nameOf(callers[i & (N - 1)], cache)]++;
		}
		// libc samples (locks, syscalls): the first frame in our own code up the chain, as an exact offset
		std::unordered_map<std::string, uint32_t> perOwn;
		{
			Dl_info me{};
			dladdr((void*)&reporter, &me);
			uintptr_t lo = (uintptr_t)me.dli_fbase;
			for (uint32_t i = tail; i != h; i++)
			{
				uint32_t j = i & (N - 1);
				const std::string& nm = nameOf(samples[j], cache);
				if (nm.compare(0, 8, "libc.so:") != 0) continue;
				uintptr_t own = 0;
				uintptr_t cand[1 + Deep] = {callers[j], deep[j][0], deep[j][1], deep[j][2], deep[j][3]};
				for (uintptr_t a : cand)
				{
					if (!a) continue;
					const std::string& an = nameOf(a, cache);
					if (an.find(':') == std::string::npos && an != "?") { own = a; break; }
				}
				char b[160];
				if (own) snprintf(b, sizeof b, "%s <= %s@%zx", nm.substr(8, 28).c_str(), nameOf(own, cache).substr(0, 40).c_str(), (size_t)(own - lo));
				else snprintf(b, sizeof b, "%s <= ?", nm.substr(8, 28).c_str());
				perOwn[b]++;
			}
		}
		tail = h;
		if (!n) continue;
		std::unordered_map<std::string, uint32_t> perFn;
		for (auto& kv : perPc) perFn[nameOf(kv.first, cache)] += kv.second;
		std::vector<std::pair<uint32_t, std::string>> v;
		for (auto& kv : perFn) v.push_back({kv.second, kv.first});
		std::sort(v.rbegin(), v.rend());
		uint32_t lifted = 0;
		for (auto& kv : perFn) if (kv.first.find("f_") != std::string::npos && kv.first.find(':') == std::string::npos) lifted += kv.second;
		rt::log("prof: %u samples (main thread CPU time), %.1f%% in lifted game code, %zu functions; top:", n, 100.0 * lifted / n, v.size());
		std::string line;
		for (size_t i = 0; i < v.size() && i < 40; i++)
		{
			char b[160]; snprintf(b, sizeof b, " %s %.1f%% |", v[i].second.c_str(), 100.0 * v[i].first / n);
			line += b;
			if ((i + 1) % 8 == 0) { rt::log("prof:%s", line.c_str()); line.clear(); }
		}
		if (!line.empty()) rt::log("prof:%s", line.c_str());
		std::vector<std::pair<uint32_t, std::string>> vc;
		for (auto& kv : perCaller) vc.push_back({kv.second, kv.first});
		std::sort(vc.rbegin(), vc.rend());
		line.clear();
		for (size_t i = 0; i < vc.size() && i < 24; i++)
		{
			char b[200]; snprintf(b, sizeof b, " %s %.1f%% |", vc[i].second.c_str(), 100.0 * vc[i].first / n);
			line += b;
			if ((i + 1) % 6 == 0) { rt::log("prof callers:%s", line.c_str()); line.clear(); }
		}
		if (!line.empty()) rt::log("prof callers:%s", line.c_str());
		std::vector<std::pair<uint32_t, std::string>> vw;
		for (auto& kv : perOwn) vw.push_back({kv.second, kv.first});
		std::sort(vw.rbegin(), vw.rend());
		line.clear();
		for (size_t i = 0; i < vw.size() && i < 24; i++)
		{
			char b[240]; snprintf(b, sizeof b, " %s %.2f%% |", vw[i].second.c_str(), 100.0 * vw[i].first / n);
			line += b;
			if ((i + 1) % 4 == 0) { rt::log("prof libc from:%s", line.c_str()); line.clear(); }
		}
		if (!line.empty()) rt::log("prof libc from:%s", line.c_str());
		// exact offsets of the hottest samples in libra3native.so code without a dynamic symbol (static functions):
		// resolved offline with the symbol list published next to the APK (ra3native-symbols.txt)
		std::unordered_map<uintptr_t, uint32_t> perOff;
		Dl_info self{};
		if (dladdr((void*)&reporter, &self) && self.dli_fbase)
			for (auto& kv : perPc)
			{
				const std::string& nm = nameOf(kv.first, cache);
				if (nm.compare(0, 16, "libra3native.so+") == 0) perOff[kv.first - (uintptr_t)self.dli_fbase] += kv.second;
			}
		std::vector<std::pair<uint32_t, uintptr_t>> vo;
		for (auto& kv : perOff) vo.push_back({kv.second, kv.first});
		std::sort(vo.rbegin(), vo.rend());
		line.clear();
		for (size_t i = 0; i < vo.size() && i < 120; i++)
		{
			char b[40]; snprintf(b, sizeof b, " %zx:%u", (size_t)vo[i].second, vo[i].first);
			line += b;
			if ((i + 1) % 12 == 0) { rt::log("prof raw:%s", line.c_str()); line.clear(); }
		}
		if (!line.empty()) rt::log("prof raw:%s", line.c_str());
	}
}
}

void startForThisThread()
{
	if (access("/sdcard/ra3_prof", F_OK) != 0) return;
	pthread_attr_t at;
	if (pthread_getattr_np(pthread_self(), &at) == 0)
	{
		void* a; size_t sz;
		if (pthread_attr_getstack(&at, &a, &sz) == 0) { stackLo = (uintptr_t)a; stackHi = (uintptr_t)a + sz; }
		pthread_attr_destroy(&at);
	}
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

// Per-thread CPU use every 60 s (also while the app is in the background, when the game thread is parked): which
// threads keep the phone busy. Only with the log file enabled.
static void threadMonitor()
{
	std::map<int, std::pair<uint64_t, std::string>> last;
	long hz = sysconf(_SC_CLK_TCK);
	if (hz <= 0) hz = 100;
	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::seconds(60));
		std::map<int, std::pair<uint64_t, std::string>> now;
		if (DIR* d = opendir("/proc/self/task"))
		{
			while (dirent* e = readdir(d))
			{
				int tid = atoi(e->d_name);
				if (tid <= 0) continue;
				char path[64], buf[512];
				snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
				FILE* f = fopen(path, "r");
				if (!f) continue;
				size_t n = fread(buf, 1, sizeof buf - 1, f);
				fclose(f);
				buf[n] = 0;
				char* lp = strchr(buf, '(');
				char* rp = strrchr(buf, ')');
				if (!lp || !rp) continue;
				std::string name(lp + 1, rp);
				unsigned long ut = 0, st = 0;
				// fields after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
				if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
				now[tid] = {(uint64_t)(ut + st), name};
			}
			closedir(d);
		}
		std::vector<std::pair<uint64_t, std::string>> v;
		uint64_t total = 0;
		for (auto& kv : now)
		{
			auto it = last.find(kv.first);
			uint64_t dt = kv.second.first - (it != last.end() ? it->second.first : 0);
			if (it == last.end() && !last.empty()) dt = 0;   // new thread: no baseline yet
			total += dt;
			if (dt) v.push_back({dt, kv.second.second + "/" + std::to_string(kv.first)});
		}
		last = std::move(now);
		std::sort(v.rbegin(), v.rend());
		std::string line;
		for (size_t i = 0; i < v.size() && i < 8; i++)
		{
			char b[96]; snprintf(b, sizeof b, " %s %.0f%%", v[i].second.c_str(), 100.0 * v[i].first / hz / 60.0);
			line += b;
		}
		rt::log("threads: %.0f%% of one core over 60 s%s:%s", 100.0 * total / hz / 60.0, ::rt::background() ? " (app in background)" : "", line.c_str());
	}
}

void profStart()
{
	prof::startForThisThread();
	if (!::rt::config.logFile.empty()) std::thread(threadMonitor).detach();
}
