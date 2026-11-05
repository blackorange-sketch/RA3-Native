// CPU state and helpers used by lifted code. Lifted functions keep registers in C locals and
// spill them into CPU around calls; the guest stack lives in guest memory.
#pragma once
#include <cstdint>

#include "guest.h"

struct CPU
{
	uint32_t r[8];      // eax ecx edx ebx esp ebp esi edi
	uint32_t fsbase;    // guest address of the TEB (fs:0)
	uint32_t df;        // direction flag, kept across calls
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
