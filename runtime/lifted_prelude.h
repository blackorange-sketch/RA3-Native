// Included at the top of generated C files.
#pragma once
#include "cpu.h"
#include "fpu.h"

#define SAVE() do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; c->df = df; } while (0)
#define SAVEFL() do { c->t_cf = cf; c->t_pf = pf; c->t_af = af; c->t_zf = zf; c->t_sf = sf; c->t_of = of; } while (0)
#define LOADFL() do { cf = c->t_cf; pf = c->t_pf; af = c->t_af; zf = c->t_zf; sf = c->t_sf; of = c->t_of; } while (0)
#define LOAD() do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; df = (uint8_t)c->df; } while (0)

// Call tracing: a per-thread ring of the last function entries, dumped on fatal errors (build with -DRA3_TRACE).
#ifdef RA3_TRACE
extern thread_local uint32_t ra3_ring[256];
extern thread_local uint32_t ra3_ring_ra[256];
extern thread_local uint32_t ra3_ring_pos;
#define TRACE_FN(a) do { uint32_t i_ = ra3_ring_pos++ & 255; ra3_ring[i_] = (a); ra3_ring_ra[i_] = G32(c->r[4]); } while (0)
#else
#define TRACE_FN(a) ((void)0)
#endif

void ra3_hook_after(CPU* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t ecx);
