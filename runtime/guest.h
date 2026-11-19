// Guest address space: a single 4 GiB host reservation. Guest address g lives at host guest_base + g.
#pragma once
#include <cstddef>
#include <cstdint>

namespace guest
{
#ifdef RA3_LIFTED_TU
// Generated code sees the base as constant (it is set once in init(), before any guest code runs). Otherwise every
// guest store, a store through uint8_t* that may alias anything, forced a reload of the base pointer before the
// next guest access: one extra dependent load per memory operation in the whole game.
extern uint8_t* const base;
#else
extern uint8_t* base;
#endif

bool init();                                   // reserve the 4 GiB window
bool commit(uint32_t addr, uint32_t size);     // make [addr, addr+size) readable/writable (zero filled)
inline uint8_t* host(uint32_t addr) { return base + addr; }

template <class T> inline T load(uint32_t a) { T v; __builtin_memcpy(&v, base + a, sizeof v); return v; }
template <class T> inline void store(uint32_t a, T v) { __builtin_memcpy(base + a, &v, sizeof v); }
}
