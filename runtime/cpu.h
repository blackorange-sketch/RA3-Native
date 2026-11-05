// CPU state and helpers used by lifted code. Lifted functions keep registers in C locals and
// spill them into CPU around calls; the guest stack lives in guest memory.
#pragma once
#include <cstdint>

#include "guest.h"

struct X128
{
	union
	{
		float f[4];
		uint32_t u[4];
		int32_t i[4];
		uint64_t q[2];
		double d[2];
		uint16_t w[8];
		uint8_t b[16];
	};
};

struct CPU
{
	uint32_t r[8];      // eax ecx edx ebx esp ebp esi edi
	uint32_t fsbase;    // guest address of the TEB (fs:0)
	uint32_t df;        // direction flag, kept across calls
	X128 xmm[8];
	double st[8];       // x87 stack as doubles (see docs/PLAN.md: precision follows the control word)
	uint32_t top = 0;   // x87 TOP
	uint16_t fcw = 0x37f;
	uint16_t fcc = 0;   // condition codes C0..C3 in their status-word positions (bits 8, 9, 10, 14)
	uint32_t mxcsr = 0x1f80;
	CPU() : r{}, fsbase(0), df(0), xmm{}, st{} {}
};

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

using LiftedFn = void (*)(CPU*);

// Indirect call/jump target resolution: lifted functions and import stubs.
void call_target(CPU* c, uint32_t guestAddr);
[[noreturn]] void unsupported(CPU* c, uint32_t va, const char* what);

#define G8(a)  (::guest::load<uint8_t>(a))
#define G16(a) (::guest::load<uint16_t>(a))
#define G32(a) (::guest::load<uint32_t>(a))
#define S8(a, v)  (::guest::store<uint8_t>((a), (uint8_t)(v)))
#define S16(a, v) (::guest::store<uint16_t>((a), (uint16_t)(v)))
#define S32(a, v) (::guest::store<uint32_t>((a), (uint32_t)(v)))

static inline uint8_t par8(uint32_t v) { return (uint8_t)(!__builtin_parity(v & 0xff)); }

#include <time.h>
static inline uint64_t rdtsc_host() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
