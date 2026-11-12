// Shared pieces of the runtime: logging, guest strings, import macros, guest calls, path mapping.
#pragma once
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "cpu.h"
#include "fpu.h"
#include "guest.h"
#include "imports.h"

namespace rt
{
// ---- configuration
struct Config
{
	std::string gameRoot = "/sdcard/RA3";   // host directory holding the game (what the guest sees as C:\RA3)
	std::string dataRoot = "/tmp/ra3-data";  // writable area for everything else (saves, options, logs)
	std::string logFile;
};
extern Config config;

extern void (*logHook)(const char*);   // optional extra sink (on-screen console, logcat)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool verbose();                 // extra diagnostics: enabled by creating /sdcard/ra3_verbose
uint64_t heapLiveBytes();
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- guest memory layout
constexpr uint32_t HeapBase = 0x01000000, HeapEnd = 0x60000000;      // malloc arena
constexpr uint32_t VmBase = 0x60000000, VmEnd = 0x6f000000;           // VirtualAlloc
constexpr uint32_t ThreadBase = 0x71000000, ThreadEnd = 0x7f000000;   // thread stacks and TEBs
constexpr uint32_t StackSize = 0x400000;
constexpr uint32_t ExitMagic = 0xfffffff0;

// ---- guest heap (heap.cpp)
void heapInit();
uint32_t gmalloc(uint32_t size, uint32_t align = 16);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gsize(uint32_t p);
uint32_t vmAlloc(uint32_t size);   // page granular, committed, zeroed

// ---- strings
std::string gstr(uint32_t a);                  // NUL-terminated byte string
std::string gstrn(uint32_t a, uint32_t n);
std::u16string gwstr(uint32_t a);
std::string utf16to8(const std::u16string& s);
std::u16string utf8to16(const std::string& s);
uint32_t putStr(uint32_t dst, const std::string& s);      // writes with NUL, returns length
uint32_t putWStr(uint32_t dst, const std::u16string& s);
uint32_t newStr(const std::string& s);                    // heap copy

// ---- calling back into guest code
void callGuest(CPU* c, uint32_t fn, std::initializer_list<uint32_t> args, int popBytes = -1);   // -1: cdecl
uint32_t callGuestRet(CPU* c, uint32_t fn, std::initializer_list<uint32_t> args, int popBytes = -1);

// ---- paths
std::string hostPath(const std::string& winPath);   // case-insensitive resolve under gameRoot / dataRoot

// ---- handles
uint32_t newHandle(void* obj, int kind);
void* getHandle(uint32_t h, int kind);
void closeHandle(uint32_t h);

// ---- threads (kernel32.cpp)
struct ThreadCtx { CPU* cpu; uint32_t tid; uint32_t teb; uint32_t stackTop; };
ThreadCtx& thisThread();
uint32_t makeTeb(uint32_t stackLow, uint32_t stackHigh, uint32_t tid);
void setLastError(uint32_t e);
void traceRegister(uint32_t tid);
void traceDump();
void profDump();
void traceDumpCurrent(int n);   // ring of the calling thread only
void watchArm(uint32_t guestAddr);   // write-protect the page; the next write is logged with the writer's call ring
bool watchHit(uint32_t guestAddr);
void traceProfile(int ms);
void callStats();
// Starts a detached host thread with a large stack (bionic's default of 1 MiB is too small for lifted code).
void spawnHostThread(std::function<void()> fn, size_t stackBytes = 64u << 20);

// ---- formatting (fmt.cpp)
struct ArgReader
{
	uint32_t addr;
	uint32_t u32() { uint32_t v = guest::load<uint32_t>(addr); addr += 4; return v; }
	uint64_t u64() { uint64_t v = guest::load<uint64_t>(addr); addr += 8; return v; }
	double f64() { double v = guest::load<double>(addr); addr += 8; return v; }
};
std::string formatA(const std::string& fmt, ArgReader& ap);
std::u16string formatW(const std::u16string& fmt, ArgReader& ap);
int scanA(const std::string& in, const std::string& fmt, ArgReader& ap, bool secure = false);

// ---- image info (set by the loader)
extern uint32_t imageBase, imageEntry;
}

#define ARG(i) (::guest::load<uint32_t>(c->r[ESP] + 4 + 4 * (i)))
#define SET(v) (c->r[EAX] = (uint32_t)(v))
#define IMP_ID(dll, nm, id, pop)                                                                 \
	static void imp_##id(CPU* c);                                                                  \
	static const struct Reg_##id { Reg_##id() { imports::registerFn(dll, nm, imp_##id, pop); } } reg_##id; \
	static void imp_##id(CPU* c)
#define IMP_STD(dll, name, nargs) IMP_ID(dll, #name, name, 4 * (nargs))
#define IMP_C(dll, name) IMP_ID(dll, #name, name, 0)
#define RET_F(x) do { fpush(c, (x)); } while (0)

namespace rt
{
// Implicit TLS template (from the PE TLS directory); set by the runner before threads start.
struct TlsInfo { uint32_t start = 0, end = 0, zeroFill = 0, index = 0; };
extern TlsInfo tls;
uint32_t newGuestStack(uint32_t& low);   // allocates a thread stack, returns the top
uint32_t spawnMainThread(CPU* c);        // sets up TEB/TLS/stack for the calling (main) thread, returns stack top
}

namespace rt { extern std::atomic<uint32_t> gLowerCallers[64], gLowerKeys[64]; }
