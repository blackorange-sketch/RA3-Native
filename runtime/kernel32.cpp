// KERNEL32 replacement: threads, synchronization, files, memory, time, misc.
#include <atomic>
#include <condition_variable>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

#include "rt.h"

#define K "KERNEL32.dll"
using namespace rt;

namespace rt
{
TlsInfo tls;
}

namespace
{
enum Kind { KFile = 1, KThread, KEvent, KMutex, KSem, KFind, KProc };

thread_local ThreadCtx tctx;
std::atomic<uint32_t> nextTid{0x1000};
std::atomic<uint32_t> bumpThread{rt::ThreadBase};
uint32_t pebAddr;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> gWaiting;   // tid -> (handle, timeout) of threads currently blocked in WaitForSingleObject
std::atomic<uint64_t> gReadCalls{0}, gReadBytes{0}, gWaitCalls{0};
std::mutex waitMu;               // one lock + condvar for all waitable objects: simple and correct
std::condition_variable waitCv;

uint32_t bump(uint32_t size)
{
	size = (size + 0xffff) & ~0xffffu;
	uint32_t a = bumpThread.fetch_add(size);
	if (a + size > rt::ThreadEnd) fatal("out of thread address space");
	guest::commit(a, size);
	return a;
}

struct Waitable
{
	Kind kind;
	bool signaled = false, manual = false;
	int count = 0, maxCount = 1;   // semaphore
	uint32_t owner = 0, depth = 0; // mutex
	bool done = false;
};
std::atomic<bool> dummy;

struct ThreadObj : Waitable
{
	std::thread th;
	uint32_t exitCode = 259;
	uint32_t tid = 0;
	uint32_t fn = 0, arg = 0;
	bool started = false, suspended = false;
};

struct FileObj { int fd; std::string path; bool dir = false; };
struct FindObj { std::vector<std::string> names; size_t pos = 0; std::string dirHost; };

uint32_t lastErrorAddr() { return thisThread().teb + 0x34; }

bool waitReady(Waitable* w, uint32_t tid)
{
	switch (w->kind)
	{
	case KMutex: if (w->owner == 0 || w->owner == tid) { w->owner = tid; w->depth++; return true; } return false;
	case KSem: if (w->count > 0) { w->count--; return true; } return false;
	default:
		if (w->signaled) { if (!w->manual) w->signaled = false; return true; }
		return false;
	}
}

Waitable* asWaitable(uint32_t h)
{
	for (int k : {KThread, KEvent, KMutex, KSem})
		if (void* p = getHandle(h, k)) return static_cast<Waitable*>(p);
	return nullptr;
}

// ---- critical sections: the guest struct stores an index into a host mutex table
std::recursive_mutex* csTable[1 << 16];
std::atomic<uint32_t> csCount{1};
std::mutex csInitMu;
std::recursive_mutex* csGet(uint32_t cs)
{
	uint32_t idx = guest::load<uint32_t>(cs);
	if (idx == 0 || idx >= (1u << 16) || !csTable[idx])
	{
		std::lock_guard<std::mutex> l(csInitMu);
		idx = guest::load<uint32_t>(cs);
		if (idx == 0 || idx >= (1u << 16) || !csTable[idx])
		{
			idx = csCount++;
			if (idx >= (1u << 16)) fatal("critical section table full");
			csTable[idx] = new std::recursive_mutex;
			guest::store<uint32_t>(cs, idx);
		}
	}
	return csTable[idx];
}

// ---- TLS slots (TlsAlloc)
std::mutex tlsMu;
bool tlsUsed[64];

void threadMain(ThreadObj* t)
{
	CPU cpu;
	uint32_t low, top = newGuestStack(low);
	tctx.cpu = &cpu;
	tctx.tid = t->tid;
	tctx.stackTop = top;
	tctx.teb = makeTeb(low, top, t->tid);
	traceRegister(t->tid);
	cpu.fsbase = tctx.teb;
	cpu.r[ESP] = top - 16;
	uint32_t r = callGuestRet(&cpu, t->fn, {t->arg}, 4);
	std::lock_guard<std::mutex> l(waitMu);
	t->exitCode = r;
	t->signaled = true;
	t->done = true;
	waitCv.notify_all();
}

std::string winTimeBase;
}

namespace rt
{
ThreadCtx& thisThread() { return tctx; }
void setLastError(uint32_t e) { if (tctx.teb) guest::store<uint32_t>(tctx.teb + 0x34, e); }

uint32_t newGuestStack(uint32_t& low)
{
	uint32_t a = bump(StackSize + 0x10000);   // TEB + stack share one block
	low = a + 0x10000;
	return a + 0x10000 + StackSize;
}

uint32_t makeTeb(uint32_t stackLow, uint32_t stackHigh, uint32_t tid)
{
	// TEB at the start of the block that precedes the stack (newGuestStack leaves 64 KiB below it)
	uint32_t teb = stackLow - 0x10000;
	memset(guest::host(teb), 0, 0x3000);
	if (!pebAddr)
	{
		pebAddr = bump(0x10000);
		guest::store<uint32_t>(pebAddr + 8, imageBase);
		guest::store<uint32_t>(pebAddr + 0xa4, 5);    // OSMajorVersion
		guest::store<uint32_t>(pebAddr + 0xa8, 1);    // OSMinorVersion (XP/7-ish: 6.1 below)
		guest::store<uint32_t>(pebAddr + 0xa4, 6);
	}
	guest::store<uint32_t>(teb + 0x00, 0xffffffff);   // SEH chain end
	guest::store<uint32_t>(teb + 0x04, stackHigh);
	guest::store<uint32_t>(teb + 0x08, stackLow);
	guest::store<uint32_t>(teb + 0x18, teb);
	guest::store<uint32_t>(teb + 0x20, getpid());
	guest::store<uint32_t>(teb + 0x24, tid);
	guest::store<uint32_t>(teb + 0x30, pebAddr);
	// implicit TLS: pointer array at +0x1000 (fs:[0x2c]); slot [tls.index] -> per-thread copy of the template
	uint32_t arr = teb + 0x1000;
	guest::store<uint32_t>(teb + 0x2c, arr);
	if (tls.end > tls.start)
	{
		uint32_t sz = tls.end - tls.start + tls.zeroFill;
		uint32_t blk = gmalloc(sz + 16);
		memset(guest::host(blk), 0, sz);
		memcpy(guest::host(blk), guest::host(tls.start), tls.end - tls.start);
		guest::store<uint32_t>(arr + 4 * tls.index, blk);
	}
	return teb;
}

uint32_t spawnMainThread(CPU* c)
{
	uint32_t low, top = newGuestStack(low);
	tctx.cpu = c;
	tctx.tid = nextTid++;
	tctx.stackTop = top;
	tctx.teb = makeTeb(low, top, tctx.tid);
	traceRegister(tctx.tid);
	c->fsbase = tctx.teb;
	c->r[ESP] = top - 16;
	return top;
}
}

// ================= threads & sync
IMP_STD(K, CreateThread, 6)
{
	auto* t = new ThreadObj;
	t->kind = KThread;
	t->manual = true;
	t->tid = nextTid++;
	t->fn = ARG(2);
	t->arg = ARG(3);
	t->suspended = (ARG(4) & 4) != 0;
	uint32_t h = newHandle(t, KThread);
	if (ARG(5)) guest::store<uint32_t>(ARG(5), t->tid);
	if (!t->suspended) { t->started = true; spawnHostThread([t] { threadMain(t); }); }
	SET(h);
}
IMP_ID("MSVCR80.dll", "_beginthreadex", beginthreadex, 0)
{
	auto* t = new ThreadObj;
	t->kind = KThread;
	t->manual = true;
	t->tid = nextTid++;
	t->fn = ARG(2);
	t->arg = ARG(3);
	t->suspended = (ARG(4) & 4) != 0;
	uint32_t h = newHandle(t, KThread);
	if (ARG(5)) guest::store<uint32_t>(ARG(5), t->tid);
	if (!t->suspended) { t->started = true; spawnHostThread([t] { threadMain(t); }); }
	SET(h);
}
IMP_ID("MSVCR80.dll", "_endthreadex", endthreadex, 0) { fatal("_endthreadex called (thread exit via unwinding not implemented)"); }
IMP_STD(K, ResumeThread, 1)
{
	auto* t = (ThreadObj*)getHandle(ARG(0), KThread);
	if (t && t->suspended && !t->started) { t->suspended = false; t->started = true; spawnHostThread([t] { threadMain(t); }); SET(1); }
	else SET(0);
}
IMP_STD(K, SuspendThread, 1) { SET(0); }
IMP_STD(K, TerminateThread, 2) { SET(1); }
IMP_STD(K, GetExitCodeThread, 2) { auto* t = (ThreadObj*)getHandle(ARG(0), KThread); if (t) guest::store<uint32_t>(ARG(1), t->done ? t->exitCode : 259); SET(t ? 1 : 0); }
IMP_STD(K, GetCurrentThreadId, 0) { SET(thisThread().tid); }
IMP_STD(K, GetCurrentProcessId, 0) { SET(getpid()); }
IMP_STD(K, GetCurrentThread, 0) { SET(0xfffffffe); }
IMP_STD(K, GetCurrentProcess, 0) { SET(0xffffffff); }
IMP_STD(K, SetThreadPriority, 2) { SET(1); }
IMP_STD(K, GetThreadPriority, 1) { SET(0); }
IMP_STD(K, SetPriorityClass, 2) { SET(1); }
IMP_STD(K, GetPriorityClass, 1) { SET(0x20); }
IMP_STD(K, SetThreadIdealProcessor, 2) { SET(0); }
IMP_STD(K, SetProcessAffinityMask, 2) { SET(1); }
IMP_STD(K, GetProcessAffinityMask, 3) { guest::store<uint32_t>(ARG(1), 0xff); guest::store<uint32_t>(ARG(2), 0xff); SET(1); }
IMP_STD(K, Sleep, 1) { std::this_thread::sleep_for(std::chrono::milliseconds(ARG(0))); }
IMP_STD(K, SleepEx, 2) { std::this_thread::sleep_for(std::chrono::milliseconds(ARG(0))); SET(0); }

IMP_STD(K, CreateEventA, 4)
{
	auto* w = new Waitable;
	w->kind = KEvent; w->manual = ARG(1) != 0; w->signaled = ARG(2) != 0;
	SET(newHandle(w, KEvent));
}
IMP_STD(K, OpenEventA, 3) { SET(0); setLastError(2); }
IMP_STD(K, SetEvent, 1) { if (auto* w = asWaitable(ARG(0))) { std::lock_guard<std::mutex> l(waitMu); w->signaled = true; waitCv.notify_all(); } SET(1); }
IMP_STD(K, ResetEvent, 1) { if (auto* w = asWaitable(ARG(0))) { std::lock_guard<std::mutex> l(waitMu); w->signaled = false; } SET(1); }
static void createMutex(CPU* c, int initialOwnerArg)
{
	auto* w = new Waitable;
	w->kind = KMutex;
	if (ARG(initialOwnerArg)) { w->owner = thisThread().tid; w->depth = 1; }
	SET(newHandle(w, KMutex));
}
IMP_STD(K, CreateMutexA, 3) { createMutex(c, 1); }
IMP_STD(K, CreateMutexW, 3) { createMutex(c, 1); }
IMP_STD(K, ReleaseMutex, 1)
{
	if (auto* w = (Waitable*)getHandle(ARG(0), KMutex))
	{
		std::lock_guard<std::mutex> l(waitMu);
		if (w->owner == thisThread().tid && --w->depth == 0) { w->owner = 0; waitCv.notify_all(); }
	}
	SET(1);
}
IMP_STD(K, CreateSemaphoreA, 4)
{
	auto* w = new Waitable;
	w->kind = KSem; w->count = (int)ARG(1); w->maxCount = (int)ARG(2);
	SET(newHandle(w, KSem));
}
IMP_STD(K, ReleaseSemaphore, 3)
{
	auto* w = (Waitable*)getHandle(ARG(0), KSem);
	if (!w) { SET(0); return; }
	std::lock_guard<std::mutex> l(waitMu);
	if (ARG(2)) guest::store<int32_t>(ARG(2), w->count);
	w->count += (int)ARG(1);
	waitCv.notify_all();
	SET(1);
}
IMP_STD(K, WaitForSingleObject, 2)
{
	Waitable* w = asWaitable(ARG(0));
	if (!w) { SET(0xffffffff); return; }
	uint32_t ms = ARG(1), tid = thisThread().tid;
	std::unique_lock<std::mutex> l(waitMu);
	gWaitCalls++;
	gWaiting[tid] = {ARG(0), ms};
	if (ms == 0xffffffff) { waitCv.wait(l, [&] { return waitReady(w, tid); }); gWaiting.erase(tid); SET(0); }
	else { bool r = waitCv.wait_for(l, std::chrono::milliseconds(ms), [&] { return waitReady(w, tid); }); gWaiting.erase(tid); SET(r ? 0 : 0x102); }
}
void dumpWaiters()
{
	std::lock_guard<std::mutex> l(waitMu);
	std::string s;
	for (auto& kv : gWaiting) { char b[64]; snprintf(b, sizeof b, " [t%u h%x ms%u]", kv.first, kv.second.first, kv.second.second); s += b; }
	rt::log("stats: reads %llu (%llu bytes), waits %llu, blocked:%s", (unsigned long long)gReadCalls, (unsigned long long)gReadBytes, (unsigned long long)gWaitCalls, s.c_str());
}
IMP_STD(K, DuplicateHandle, 7) { guest::store<uint32_t>(ARG(3), ARG(1)); SET(1); }
IMP_STD(K, CloseHandle, 1)
{
	uint32_t h = ARG(0);
	if (auto* f = (FileObj*)getHandle(h, KFile)) { if (f->fd >= 0) close(f->fd); delete f; }
	closeHandle(h);
	SET(1);
}

IMP_STD(K, InitializeCriticalSection, 1) { guest::store<uint32_t>(ARG(0), 0); csGet(ARG(0)); }
IMP_STD(K, InitializeCriticalSectionAndSpinCount, 2) { guest::store<uint32_t>(ARG(0), 0); csGet(ARG(0)); SET(1); }
IMP_STD(K, DeleteCriticalSection, 1) {}
IMP_STD(K, EnterCriticalSection, 1) { csGet(ARG(0))->lock(); }
IMP_STD(K, LeaveCriticalSection, 1) { csGet(ARG(0))->unlock(); }
IMP_STD(K, TryEnterCriticalSection, 1) { SET(csGet(ARG(0))->try_lock() ? 1 : 0); }

IMP_STD(K, InterlockedIncrement, 1) { SET(__atomic_add_fetch((int32_t*)guest::host(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
IMP_STD(K, InterlockedDecrement, 1) { SET(__atomic_sub_fetch((int32_t*)guest::host(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
IMP_STD(K, InterlockedExchange, 2) { SET(__atomic_exchange_n((int32_t*)guest::host(ARG(0)), (int32_t)ARG(1), __ATOMIC_SEQ_CST)); }
IMP_STD(K, InterlockedExchangeAdd, 2) { SET(__atomic_fetch_add((int32_t*)guest::host(ARG(0)), (int32_t)ARG(1), __ATOMIC_SEQ_CST)); }
IMP_STD(K, InterlockedCompareExchange, 3)
{
	int32_t exp = (int32_t)ARG(2);
	__atomic_compare_exchange_n((int32_t*)guest::host(ARG(0)), &exp, (int32_t)ARG(1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	SET(exp);
}

IMP_STD(K, TlsAlloc, 0)
{
	std::lock_guard<std::mutex> l(tlsMu);
	for (uint32_t i = 1; i < 64; i++) if (!tlsUsed[i]) { tlsUsed[i] = true; SET(i + 32); return; }   // 0..31 left for implicit TLS
	SET(0xffffffff);
}
IMP_STD(K, TlsFree, 1) { std::lock_guard<std::mutex> l(tlsMu); if (ARG(0) >= 32 && ARG(0) < 96) tlsUsed[ARG(0) - 32] = false; SET(1); }
IMP_STD(K, TlsSetValue, 2) { guest::store<uint32_t>(thisThread().teb + 0x1000 + 4 * ARG(0), ARG(1)); SET(1); }
IMP_STD(K, TlsGetValue, 1) { SET(guest::load<uint32_t>(thisThread().teb + 0x1000 + 4 * ARG(0))); }
IMP_STD(K, GetLastError, 0) { SET(guest::load<uint32_t>(lastErrorAddr())); }
IMP_STD(K, SetLastError, 1) { setLastError(ARG(0)); }
IMP_STD(K, RaiseException, 4) { if (ARG(0) == 0x406d1388) return; fatal("RaiseException(code %08x)", ARG(0)); }
IMP_STD(K, SetUnhandledExceptionFilter, 1) { SET(0); }
IMP_STD(K, UnhandledExceptionFilter, 1) { SET(0); }
IMP_STD(K, IsDebuggerPresent, 0) { SET(0); }
IMP_STD(K, OutputDebugStringA, 1) { log("[dbg] %s", gstr(ARG(0)).c_str()); }
IMP_STD(K, SetErrorMode, 1) { SET(0); }

// ================= memory
IMP_STD(K, VirtualAlloc, 4)
{
	uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
	if (addr && addr >= VmBase && addr < VmEnd && size && !(type & 0x2000)) { SET(addr); return; }
	SET(vmAlloc(size));
}
IMP_STD(K, VirtualFree, 3) { SET(1); }
IMP_STD(K, VirtualQuery, 3)
{
	uint32_t p = ARG(0), out = ARG(1);
	memset(guest::host(out), 0, 28);
	guest::store<uint32_t>(out, p & ~0xfffu);
	guest::store<uint32_t>(out + 4, p & ~0xfffu);
	guest::store<uint32_t>(out + 12, 0x1000);   // RegionSize
	guest::store<uint32_t>(out + 16, 0x1000);   // MEM_COMMIT
	guest::store<uint32_t>(out + 20, 4);        // PAGE_READWRITE
	SET(28);
}
IMP_STD(K, VirtualProtect, 4) { if (ARG(3)) guest::store<uint32_t>(ARG(3), 4); SET(1); }
IMP_STD(K, IsBadReadPtr, 2) { SET(ARG(0) == 0 ? 1 : 0); }
IMP_STD(K, IsBadWritePtr, 2) { SET(ARG(0) == 0 ? 1 : 0); }
IMP_STD(K, IsBadCodePtr, 1) { SET(ARG(0) == 0 ? 1 : 0); }
IMP_STD(K, GetProcessHeap, 0) { SET(0x50000); }
IMP_STD(K, HeapAlloc, 3) { uint32_t p = gmalloc(ARG(2)); if (ARG(1) & 8) memset(guest::host(p), 0, ARG(2)); SET(p); }
IMP_STD(K, HeapFree, 3) { gfree(ARG(2)); SET(1); }
IMP_STD(K, HeapReAlloc, 4) { SET(grealloc(ARG(2), ARG(3))); }
IMP_STD(K, HeapSize, 3) { SET(gsize(ARG(2))); }
IMP_STD(K, HeapCreate, 3) { SET(0x50000); }
IMP_STD(K, HeapDestroy, 1) { SET(1); }
IMP_STD(K, GlobalAlloc, 2) { uint32_t p = gmalloc(ARG(1)); if (ARG(0) & 0x40) memset(guest::host(p), 0, ARG(1)); SET(p); }
IMP_STD(K, GlobalFree, 1) { gfree(ARG(0)); SET(0); }
IMP_STD(K, GlobalLock, 1) { SET(ARG(0)); }
IMP_STD(K, GlobalUnlock, 1) { SET(0); }
IMP_STD(K, GlobalSize, 1) { SET(gsize(ARG(0))); }
IMP_STD(K, GlobalReAlloc, 3) { SET(grealloc(ARG(0), ARG(1))); }
IMP_STD(K, LocalAlloc, 2) { uint32_t p = gmalloc(ARG(1)); if (ARG(0) & 0x40) memset(guest::host(p), 0, ARG(1)); SET(p); }
IMP_STD(K, LocalFree, 1) { gfree(ARG(0)); SET(0); }
IMP_STD(K, GlobalMemoryStatus, 1)
{
	uint32_t p = ARG(0);
	guest::store<uint32_t>(p, 32);
	guest::store<uint32_t>(p + 4, 30);
	guest::store<uint32_t>(p + 8, 0x7f000000);    // total phys
	guest::store<uint32_t>(p + 12, 0x60000000);   // avail phys
	guest::store<uint32_t>(p + 16, 0x7fff0000);
	guest::store<uint32_t>(p + 20, 0x60000000);
	guest::store<uint32_t>(p + 24, 0x7fff0000);
	guest::store<uint32_t>(p + 28, 0x60000000);
}
IMP_STD(K, GlobalMemoryStatusEx, 1)
{
	uint32_t p = ARG(0);
	memset(guest::host(p), 0, 64);
	guest::store<uint32_t>(p, 64);
	guest::store<uint32_t>(p + 4, 30);
	guest::store<uint64_t>(p + 8, 0x7f000000ull);
	guest::store<uint64_t>(p + 16, 0x60000000ull);
	guest::store<uint64_t>(p + 24, 0x7fff0000ull);
	guest::store<uint64_t>(p + 32, 0x60000000ull);
	guest::store<uint64_t>(p + 40, 0x7fff0000ull);
	guest::store<uint64_t>(p + 48, 0x60000000ull);
	SET(1);
}
IMP_STD(K, ReadProcessMemory, 5) { SET(0); }

// ================= time
static uint64_t winFileTime()
{
	timespec t; clock_gettime(CLOCK_REALTIME, &t);
	return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + t.tv_nsec / 100;
}
IMP_STD(K, GetTickCount, 0) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); SET((uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000)); }
IMP_ID("WINMM.dll", "timeGetTime", timeGetTime, 0) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); SET((uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000)); }
IMP_ID("WINMM.dll", "timeBeginPeriod", timeBeginPeriod, 4) { SET(0); }
IMP_ID("WINMM.dll", "timeEndPeriod", timeEndPeriod, 4) { SET(0); }
IMP_STD(K, QueryPerformanceFrequency, 1) { guest::store<uint64_t>(ARG(0), 1000000000ull); SET(1); }
IMP_STD(K, QueryPerformanceCounter, 1) { guest::store<uint64_t>(ARG(0), rdtsc_host()); SET(1); }
IMP_STD(K, GetSystemTimeAsFileTime, 1) { guest::store<uint64_t>(ARG(0), winFileTime()); }
IMP_STD(K, GetLocalTime, 1)
{
	time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
	uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0};
	for (int i = 0; i < 8; i++) guest::store<uint16_t>(ARG(0) + 2 * i, v[i]);
}
IMP_STD(K, GetTimeZoneInformation, 1) { memset(guest::host(ARG(0)), 0, 172); SET(0); }
IMP_STD(K, GetTimeFormatA, 6) { SET(putStr(ARG(4), "00:00:00") + 1); }
IMP_STD(K, FileTimeToDosDateTime, 3) { guest::store<uint16_t>(ARG(1), 0x21); guest::store<uint16_t>(ARG(2), 0); SET(1); }
IMP_STD(K, DosDateTimeToFileTime, 3) { guest::store<uint64_t>(ARG(2), winFileTime()); SET(1); }

// ================= process / module / system
IMP_STD(K, GetVersionExA, 1)
{
	uint32_t p = ARG(0);
	guest::store<uint32_t>(p + 4, 6);
	guest::store<uint32_t>(p + 8, 1);
	guest::store<uint32_t>(p + 12, 7601);
	guest::store<uint32_t>(p + 16, 2);
	putStr(p + 20, "Service Pack 1");
	SET(1);
}
IMP_STD(K, GetSystemInfo, 1)
{
	uint32_t p = ARG(0);
	memset(guest::host(p), 0, 36);
	guest::store<uint32_t>(p + 4, 0x1000);
	guest::store<uint32_t>(p + 8, 0x10000);
	guest::store<uint32_t>(p + 12, 0x7ffeffff);
	guest::store<uint32_t>(p + 20, 4);          // processors
	guest::store<uint32_t>(p + 24, 586);
	guest::store<uint32_t>(p + 28, 0x10000);
}
IMP_STD(K, GetModuleHandleA, 1) { SET(ARG(0) ? 0x7ff00000 : imageBase); }
IMP_STD(K, GetModuleFileNameA, 3) { std::string s = "C:\\RA3\\Data\\ra3_1.12.game"; uint32_t n = std::min<uint32_t>(s.size(), ARG(2) - 1); guest::store<uint8_t>(ARG(1) + n, 0); memcpy(guest::host(ARG(1)), s.data(), n); SET(n); }
IMP_STD(K, GetModuleFileNameW, 3) { auto s = utf8to16("C:\\RA3\\Data\\ra3_1.12.game"); uint32_t n = std::min<uint32_t>(s.size(), ARG(2) - 1); guest::store<char16_t>(ARG(1) + 2 * n, 0); memcpy(guest::host(ARG(1)), s.data(), 2 * n); SET(n); }
IMP_STD(K, GetCommandLineA, 0) { static uint32_t a = newStr("\"C:\\RA3\\Data\\ra3_1.12.game\" -config RA3_russian_1.12.SkuDef -win"); SET(a); }
IMP_STD(K, GetCommandLineW, 0) { static uint32_t a = [] { auto s = utf8to16("\"C:\\RA3\\Data\\ra3_1.12.game\" -config RA3_russian_1.12.SkuDef -win"); uint32_t p = gmalloc(2 * s.size() + 2); putWStr(p, s); return p; }(); SET(a); }
IMP_STD(K, GetStartupInfoA, 1) { memset(guest::host(ARG(0)), 0, 68); guest::store<uint32_t>(ARG(0), 68); }
IMP_STD(K, GetEnvironmentVariableA, 3) { SET(0); setLastError(203); }
IMP_STD(K, SetEnvironmentVariableA, 2) { SET(1); }
IMP_STD(K, GetComputerNameA, 2) { putStr(ARG(0), "ANDROID"); guest::store<uint32_t>(ARG(1), 7); SET(1); }
IMP_ID("ADVAPI32.dll", "GetUserNameA", GetUserNameA, 8) { putStr(ARG(0), "player"); guest::store<uint32_t>(ARG(1), 7); SET(1); }
IMP_STD(K, LoadLibraryA, 1) { std::string n = gstr(ARG(0)); log("LoadLibraryA(%s)", n.c_str()); SET(0); setLastError(126); }
IMP_STD(K, FreeLibrary, 1) { SET(1); }
IMP_STD(K, GetProcAddress, 2) { SET(0); setLastError(127); }
IMP_STD(K, GetLogicalDrives, 0) { SET(1 << 2); }
IMP_STD(K, GetWindowsDirectoryW, 2) { SET(putWStr(ARG(0), u"C:\\Windows")); }
IMP_STD(K, GetTempPathA, 2) { SET(putStr(ARG(1), "C:\\Temp\\")); }
IMP_STD(K, GetTempFileNameA, 4) { static int n; SET(putStr(ARG(3), gstr(ARG(0)) + "\\tmp" + std::to_string(++n) + ".tmp")); }
IMP_STD(K, GetCurrentDirectoryA, 2) { SET(putStr(ARG(1), "C:\\RA3")); }
IMP_STD(K, SetCurrentDirectoryW, 1) { SET(1); }
IMP_STD(K, GetLocaleInfoA, 4) { SET(putStr(ARG(2), "1") + 1); }
IMP_STD(K, GetLocaleInfoW, 4) { SET(putWStr(ARG(2), u"1") + 1); }
IMP_STD(K, GetACP, 0) { SET(1251); }
IMP_STD(K, GetOEMCP, 0) { SET(866); }
IMP_STD(K, GetUserDefaultLCID, 0) { SET(0x419); }
IMP_STD(K, GetSystemDefaultLCID, 0) { SET(0x419); }
IMP_STD(K, GetStdHandle, 1) { SET(0x10 + (0xfffffff6u - ARG(0))); }
IMP_STD(K, FreeConsole, 0) { SET(1); }
IMP_STD(K, GetNumberOfConsoleInputEvents, 2) { SET(0); }
IMP_STD(K, ReadConsoleInputA, 4) { SET(0); }
IMP_STD(K, GetConsoleScreenBufferInfo, 2) { SET(0); }
IMP_STD(K, GetLargestConsoleWindowSize, 1) { SET(0); }
IMP_STD(K, SetConsoleScreenBufferSize, 2) { SET(0); }
IMP_STD(K, WriteConsoleOutputA, 5) { SET(0); }
IMP_STD(K, SetNamedPipeHandleState, 4) { SET(0); }
IMP_STD(K, CreateToolhelp32Snapshot, 2) { SET(0xffffffff); }
IMP_STD(K, Thread32First, 2) { SET(0); }
IMP_STD(K, Thread32Next, 2) { SET(0); }
IMP_ID("USER32.dll", "EnumThreadWindows", EnumThreadWindows, 12) { SET(1); }
IMP_STD(K, MultiByteToWideChar, 6)
{
	uint32_t srcLen = ARG(3);
	std::string s = srcLen == 0xffffffffu ? gstr(ARG(2)) + std::string(1, '\0') : gstrn(ARG(2), srcLen);
	std::u16string w;
	if (ARG(0) == 65001) w = utf8to16(s);
	else for (unsigned char ch : s) w += (char16_t)(ch >= 0xc0 && ARG(0) == 1251 ? 0x410 + (ch - 0xc0) : ch);
	if (srcLen == 0xffffffffu && w.empty()) w = u"\0";
	if (ARG(5) == 0) { SET(w.size()); return; }
	if (w.size() > ARG(5)) { setLastError(122); SET(0); return; }
	memcpy(guest::host(ARG(4)), w.data(), 2 * w.size());
	SET(w.size());
}
IMP_STD(K, WideCharToMultiByte, 8)
{
	uint32_t srcLen = ARG(3);
	std::u16string w = srcLen == 0xffffffffu ? gwstr(ARG(2)) + std::u16string(1, u'\0') : std::u16string((char16_t*)guest::host(ARG(2)), srcLen);
	std::string s;
	if (ARG(0) == 65001) { s = utf16to8(w); if (srcLen == 0xffffffffu) s += '\0'; }
	else for (char16_t ch : w) s += ch < 0x100 ? (char)ch : (ch >= 0x410 && ch <= 0x44f && ARG(0) == 1251 ? (char)(0xc0 + ch - 0x410) : '?');
	if (ARG(5) == 0) { SET(s.size()); return; }
	if (s.size() > ARG(5)) { setLastError(122); SET(0); return; }
	memcpy(guest::host(ARG(4)), s.data(), s.size());
	SET(s.size());
}


static std::string lowerStr(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
// Debug aid: dump interesting entries of an EA BIG archive that the game has just opened.
static void scanBig(int fd, const std::string& name)
{
	uint8_t h[16];
	if (pread(fd, h, 16, 0) != 16 || memcmp(h, "BIG", 3) != 0) { log("big %s: bad header", name.c_str()); return; }
	auto be = [](const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
	uint32_t count = be(h + 8), first = be(h + 12);
	std::vector<uint8_t> toc(first > 16 && first < (64u << 20) ? first - 16 : 0);
	if (toc.empty() || pread(fd, toc.data(), toc.size(), 16) != (ssize_t)toc.size()) { log("big %s: toc read failed", name.c_str()); return; }
	size_t pos = 0;
	uint32_t shown = 0;
	for (uint32_t i = 0; i < count && pos + 9 <= toc.size(); i++)
	{
		uint32_t off = be(&toc[pos]), sz = be(&toc[pos + 4]);
		const char* n = (const char*)&toc[pos + 8];
		size_t len = strnlen(n, toc.size() - pos - 8);
		pos += 8 + len + 1;
		std::string ln = lowerStr(std::string(n, len));
		if ((ln.find("manifest") != std::string::npos || ln.find(".version") != std::string::npos) && shown++ < 12)
			log("big %s: [%s] off=%u size=%u", name.c_str(), std::string(n, len).c_str(), off, sz);
	}
	log("big %s: %u entries, first data offset %u", name.c_str(), count, first);
}

// ================= files
// Intro/cutscene videos are not supported yet (decoder output never reaches a texture): hide them so the game skips them.
static bool isMovieFile(const std::string& p)
{
	std::string l;
	for (char ch : p) l += ch == '\\' ? '/' : (char)tolower((unsigned char)ch);
	if (getenv("RA3_PLAY_MOVIES")) return false;
	return l.find("/movies/") != std::string::npos && (l.size() > 4 && (l.compare(l.size() - 4, 4, ".vp6") == 0 || l.compare(l.size() - 4, 4, ".snd") == 0));
}
static FileObj* fileOf(uint32_t h) { return (FileObj*)getHandle(h, KFile); }
static void openFile(CPU* c, const std::string& winPath, uint32_t access, uint32_t share, uint32_t disp)
{
	(void)share;
	if (isMovieFile(winPath)) { log("open hidden (movie): '%s'", winPath.c_str()); setLastError(2); SET(0xffffffff); return; }
	std::string hp = hostPath(winPath);
	int flags = (access & 0x40000000) ? ((access & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
	switch (disp)
	{
	case 1: flags |= O_CREAT | O_EXCL; break;
	case 2: flags |= O_CREAT | O_TRUNC; break;
	case 3: break;
	case 4: flags |= O_CREAT; break;
	case 5: flags |= O_TRUNC; break;
	}
	if (flags & O_CREAT)
	{
		size_t k = hp.rfind('/');
		if (k != std::string::npos)
			for (size_t i = 1; i <= k; i++) if (hp[i] == '/' || i == k) mkdir(hp.substr(0, i).c_str(), 0777);
	}
	struct stat st;
	bool isDir = stat(hp.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
	int fd = isDir ? -1 : open(hp.c_str(), flags, 0666);
	if (fd < 0 && !isDir) { if (getenv("RA3_RINGDUMP") && winPath.find("audio.manifest") != std::string::npos) traceDump();
	if (!(flags & O_CREAT)) log("open failed: '%s' -> '%s' (errno %d)", winPath.c_str(), hp.c_str(), errno); setLastError(errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5); SET(0xffffffff); return; }
	setLastError(disp == 4 && fd >= 0 && st.st_size > 0 ? 183 : 0);
	{
		static int opened = 0;
		if (opened++ < 300) log("open ok: '%s' -> %s%s acc=%08x disp=%u fd=%d", winPath.c_str(), hp.c_str(), isDir ? " (dir)" : "", access, disp, fd);
	}
	if (fd >= 0 && !(flags & (O_WRONLY | O_RDWR)) && hp.size() > 7 && lowerStr(hp.substr(hp.size() - 7)) == ".skudef")
	{
		char buf[900];
		ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
		std::string t;
		for (ssize_t i = 0; i < n; i++) t += buf[i] == '\n' ? std::string(" | ") : buf[i] == '\r' ? std::string() : std::string(1, buf[i]);
		log("skudef %s: %s", hp.substr(hp.rfind('/') + 1).c_str(), t.c_str());
	}
	if (fd >= 0 && !(flags & (O_WRONLY | O_RDWR)) && hp.size() > 4 && lowerStr(hp.substr(hp.size() - 4)) == ".big")
	{
		static int scanned = 0;
		if (scanned++ < 60) scanBig(fd, hp.substr(hp.rfind('/') + 1));
	}
	auto* f = new FileObj{fd, hp, isDir};
	SET(newHandle(f, KFile));
}
IMP_STD(K, CreateFileA, 7) { openFile(c, gstr(ARG(0)), ARG(1), ARG(2), ARG(4)); }
IMP_STD(K, CreateFileW, 7) { openFile(c, utf16to8(gwstr(ARG(0))), ARG(1), ARG(2), ARG(4)); }
IMP_STD(K, ReadFile, 5)
{
	FileObj* f = fileOf(ARG(0));
	if (!f || f->fd < 0) { SET(0); return; }
	ssize_t n = read(f->fd, guest::host(ARG(1)), ARG(2));
	{
		static int reads = 0;
		if (reads++ < 150 || n < 0 || (uint32_t)n != ARG(2))
			if (reads < 600) log("ReadFile fd=%d want=%u got=%d ovl=%08x errno=%d", f->fd, ARG(2), (int)n, ARG(4), n < 0 ? errno : 0);
	}
	if (n < 0) { SET(0); return; }
	gReadCalls++; gReadBytes += (uint64_t)n;
	if (ARG(3)) guest::store<uint32_t>(ARG(3), (uint32_t)n);
	SET(1);
}
IMP_STD(K, WriteFile, 5)
{
	uint32_t h = ARG(0);
	if (h >= 0x10 && h <= 0x12) { fwrite(guest::host(ARG(1)), 1, ARG(2), h == 0x11 ? stdout : stderr); if (ARG(3)) guest::store<uint32_t>(ARG(3), ARG(2)); SET(1); return; }
	FileObj* f = fileOf(h);
	if (!f || f->fd < 0) { SET(0); return; }
	ssize_t n = write(f->fd, guest::host(ARG(1)), ARG(2));
	if (n < 0) { SET(0); return; }
	if (ARG(3)) guest::store<uint32_t>(ARG(3), (uint32_t)n);
	SET(1);
}
IMP_STD(K, SetFilePointer, 4)
{
	FileObj* f = fileOf(ARG(0));
	if (!f) { SET(0xffffffff); return; }
	int64_t off = (int32_t)ARG(1);
	if (ARG(2)) off |= (int64_t)(int32_t)guest::load<uint32_t>(ARG(2)) << 32;
	off_t r = lseek(f->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
	if (ARG(2)) guest::store<uint32_t>(ARG(2), (uint32_t)((uint64_t)r >> 32));
	SET((uint32_t)r);
}
IMP_STD(K, SetFilePointerEx, 5)
{
	FileObj* f = fileOf(ARG(0));
	if (!f) { SET(0); return; }
	int64_t off = (int64_t)(ARG(1) | (uint64_t)ARG(2) << 32);
	off_t r = lseek(f->fd, off, ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END);
	if (ARG(3)) guest::store<int64_t>(ARG(3), r);
	SET(1);
}
IMP_STD(K, GetFileSize, 2)
{
	FileObj* f = fileOf(ARG(0));
	struct stat st;
	if (!f || fstat(f->fd, &st)) { SET(0xffffffff); return; }
	if (ARG(1)) guest::store<uint32_t>(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
	SET((uint32_t)st.st_size);
}
IMP_STD(K, GetFileSizeEx, 2) { FileObj* f = fileOf(ARG(0)); struct stat st; if (!f || fstat(f->fd, &st)) { SET(0); return; } guest::store<int64_t>(ARG(1), st.st_size); SET(1); }
IMP_STD(K, SetEndOfFile, 1) { FileObj* f = fileOf(ARG(0)); SET(f && ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0); }
IMP_STD(K, FlushFileBuffers, 1) { SET(1); }
IMP_STD(K, SetFileTime, 4) { SET(1); }
static uint64_t toFileTime(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }
IMP_STD(K, GetFileInformationByHandle, 2)
{
	FileObj* f = fileOf(ARG(0));
	struct stat st;
	if (!f || fstat(f->fd, &st)) { SET(0); return; }
	uint32_t p = ARG(1);
	memset(guest::host(p), 0, 52);
	guest::store<uint32_t>(p, 0x80);
	guest::store<uint64_t>(p + 4, toFileTime(st.st_ctime));
	guest::store<uint64_t>(p + 12, toFileTime(st.st_atime));
	guest::store<uint64_t>(p + 20, toFileTime(st.st_mtime));
	guest::store<uint32_t>(p + 36, (uint32_t)((uint64_t)st.st_size >> 32));
	guest::store<uint32_t>(p + 40, (uint32_t)st.st_size);
	guest::store<uint32_t>(p + 44, 1);
	SET(1);
}
IMP_STD(K, DeleteFileA, 1) { SET(unlink(hostPath(gstr(ARG(0))).c_str()) == 0); }
IMP_STD(K, DeleteFileW, 1) { SET(unlink(hostPath(utf16to8(gwstr(ARG(0)))).c_str()) == 0); }
IMP_STD(K, RemoveDirectoryA, 1) { SET(rmdir(hostPath(gstr(ARG(0))).c_str()) == 0); }
IMP_STD(K, RemoveDirectoryW, 1) { SET(rmdir(hostPath(utf16to8(gwstr(ARG(0)))).c_str()) == 0); }
IMP_STD(K, CreateDirectoryW, 2) { SET(mkdir(hostPath(utf16to8(gwstr(ARG(0)))).c_str(), 0777) == 0); }
IMP_STD(K, MoveFileW, 2) { SET(rename(hostPath(utf16to8(gwstr(ARG(0)))).c_str(), hostPath(utf16to8(gwstr(ARG(1)))).c_str()) == 0); }
IMP_STD(K, CopyFileA, 3)
{
	FILE* a = fopen(hostPath(gstr(ARG(0))).c_str(), "rb");
	if (!a) { SET(0); return; }
	FILE* b = fopen(hostPath(gstr(ARG(1))).c_str(), "wb");
	if (!b) { fclose(a); SET(0); return; }
	char buf[65536]; size_t n;
	while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
	fclose(a); fclose(b);
	SET(1);
}
static void diskFree(CPU* c, uint32_t pFree, uint32_t pTotal, uint32_t pTotalFree)
{
	struct statvfs sv{};
	statvfs(config.dataRoot.c_str(), &sv);
	uint64_t fr = (uint64_t)sv.f_bavail * sv.f_frsize, tot = (uint64_t)sv.f_blocks * sv.f_frsize;
	if (!tot) { fr = 8ull << 30; tot = 64ull << 30; }
	if (pFree) guest::store<uint64_t>(pFree, fr);
	if (pTotal) guest::store<uint64_t>(pTotal, tot);
	if (pTotalFree) guest::store<uint64_t>(pTotalFree, fr);
	SET(1);
}
IMP_STD(K, GetDiskFreeSpaceExA, 4) { diskFree(c, ARG(1), ARG(2), ARG(3)); }
IMP_STD(K, GetDiskFreeSpaceExW, 4) { diskFree(c, ARG(1), ARG(2), ARG(3)); }

static void fillFind(uint32_t out, const std::string& hp, const std::string& name, bool wide)
{
	struct stat st{};
	stat((hp + "/" + name).c_str(), &st);
	memset(guest::host(out), 0, wide ? 592 : 320);
	guest::store<uint32_t>(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
	guest::store<uint64_t>(out + 4, toFileTime(st.st_ctime));
	guest::store<uint64_t>(out + 12, toFileTime(st.st_atime));
	guest::store<uint64_t>(out + 20, toFileTime(st.st_mtime));
	guest::store<uint32_t>(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
	guest::store<uint32_t>(out + 32, (uint32_t)st.st_size);
	if (wide) putWStr(out + 44, utf8to16(name)); else putStr(out + 44, name);
}
static bool wildMatch(const char* p, const char* s)
{
	if (!*p) return !*s;
	if (*p == '*') return wildMatch(p + 1, s) || (*s && wildMatch(p, s + 1));
	if (*s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))) return wildMatch(p + 1, s + 1);
	return false;
}
static void findFirst(CPU* c, const std::string& winPattern, uint32_t out, bool wide)
{
	size_t k = winPattern.find_last_of("\\/");
	std::string dirWin = k == std::string::npos ? "." : winPattern.substr(0, k), pat = k == std::string::npos ? winPattern : winPattern.substr(k + 1);
	std::string hp = hostPath(dirWin);
	auto* fo = new FindObj;
	fo->dirHost = hp;
	if (isMovieFile(winPattern)) {}
	else if (DIR* d = opendir(hp.c_str()))
	{
		while (auto* e = readdir(d)) if (wildMatch(pat.c_str(), e->d_name)) fo->names.push_back(e->d_name);
		closedir(d);
	}
	{
		static int finds = 0;
		if (finds++ < 100) log("FindFirst '%s' -> '%s' : %d matches", winPattern.c_str(), hp.c_str(), (int)fo->names.size());
	}
	if (fo->names.empty()) { delete fo; setLastError(2); SET(0xffffffff); return; }
	fillFind(out, hp, fo->names[fo->pos++], wide);
	SET(newHandle(fo, KFind));
}
IMP_STD(K, FindFirstFileA, 2) { findFirst(c, gstr(ARG(0)), ARG(1), false); }
IMP_STD(K, FindFirstFileW, 2) { findFirst(c, utf16to8(gwstr(ARG(0))), ARG(1), true); }
IMP_STD(K, FindNextFileW, 2)
{
	auto* fo = (FindObj*)getHandle(ARG(0), KFind);
	if (!fo || fo->pos >= fo->names.size()) { setLastError(18); SET(0); return; }
	fillFind(ARG(1), fo->dirHost, fo->names[fo->pos++], true);
	SET(1);
}
IMP_STD(K, FindNextFileA, 2)
{
	auto* fo = (FindObj*)getHandle(ARG(0), KFind);
	if (!fo || fo->pos >= fo->names.size()) { setLastError(18); SET(0); return; }
	fillFind(ARG(1), fo->dirHost, fo->names[fo->pos++], false);
	SET(1);
}
IMP_STD(K, lstrcpyW, 2)
{
	uint32_t d = ARG(0), sp = ARG(1), i = 0;
	for (;; i++) { uint16_t ch = guest::load<uint16_t>(sp + 2 * i); guest::store<uint16_t>(d + 2 * i, ch); if (!ch) break; }
	SET(d);
}
IMP_STD(K, ExitProcess, 1) { log("ExitProcess(%u)", ARG(0)); traceDump(); fflush(nullptr); _exit((int)ARG(0)); }
IMP_STD(K, TerminateProcess, 2) { log("TerminateProcess(%u)", ARG(1)); traceDump(); fflush(nullptr); _exit((int)ARG(1)); }
IMP_STD(K, AllocConsole, 0) { SET(1); }
IMP_STD(K, SetConsoleTitleA, 1) { SET(1); }
IMP_STD(K, GetConsoleTitleA, 2) { if (ARG(1)) guest::store<uint8_t>(ARG(0), 0); SET(0); }
IMP_STD(K, SetConsoleCtrlHandler, 2) { SET(1); }
IMP_STD(K, SetConsoleCursorInfo, 2) { SET(1); }
IMP_STD(K, SetConsoleMode, 2) { SET(1); }
IMP_STD(K, SetConsoleWindowInfo, 3) { SET(1); }
IMP_STD(K, GetDiskFreeSpaceA, 5)
{
	if (ARG(1)) guest::store<uint32_t>(ARG(1), 8);
	if (ARG(2)) guest::store<uint32_t>(ARG(2), 512);
	if (ARG(3)) guest::store<uint32_t>(ARG(3), 4000000);
	if (ARG(4)) guest::store<uint32_t>(ARG(4), 8000000);
	SET(1);
}
IMP_STD(K, FindClose, 1) { if (auto* fo = (FindObj*)getHandle(ARG(0), KFind)) { delete fo; closeHandle(ARG(0)); } SET(1); }
