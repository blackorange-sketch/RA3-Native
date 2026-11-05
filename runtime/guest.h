// Guest address space: a single 4 GiB host reservation. Guest address g lives at host guest_base + g.
#pragma once
#include <cstddef>
#include <cstdint>

namespace guest
{
extern uint8_t* base;

bool init();                                   // reserve the 4 GiB window
bool commit(uint32_t addr, uint32_t size);     // make [addr, addr+size) readable/writable (zero filled)
inline uint8_t* host(uint32_t addr) { return base + addr; }

template <class T> inline T load(uint32_t a) { T v; __builtin_memcpy(&v, base + a, sizeof v); return v; }
template <class T> inline void store(uint32_t a, T v) { __builtin_memcpy(base + a, &v, sizeof v); }
}
