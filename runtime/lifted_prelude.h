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

// ---- MMX (mm registers live in CPU::mm; the x87 aliasing is not modelled, EMMS is a no-op)
union MMU { uint64_t q; uint8_t b[8]; int8_t sb[8]; uint16_t w[4]; int16_t sw[4]; uint32_t d[2]; int32_t sd[2]; };
static inline int16_t mm_s16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int8_t mm_s8(int v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline uint8_t mm_u8(int v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t mm_u16(int v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }
#define MMF(name, body) static inline uint64_t name(uint64_t a_, uint64_t b_) { MMU a, b, r; a.q = a_; b.q = b_; r.q = 0; body; return r.q; }
MMF(mm_paddb, for (int i = 0; i < 8; i++) r.b[i] = a.b[i] + b.b[i];)
MMF(mm_paddw, for (int i = 0; i < 4; i++) r.w[i] = a.w[i] + b.w[i];)
MMF(mm_paddd, for (int i = 0; i < 2; i++) r.d[i] = a.d[i] + b.d[i];)
MMF(mm_psubb, for (int i = 0; i < 8; i++) r.b[i] = a.b[i] - b.b[i];)
MMF(mm_psubw, for (int i = 0; i < 4; i++) r.w[i] = a.w[i] - b.w[i];)
MMF(mm_psubd, for (int i = 0; i < 2; i++) r.d[i] = a.d[i] - b.d[i];)
MMF(mm_paddsb, for (int i = 0; i < 8; i++) r.sb[i] = mm_s8(a.sb[i] + b.sb[i]);)
MMF(mm_paddsw, for (int i = 0; i < 4; i++) r.sw[i] = mm_s16(a.sw[i] + b.sw[i]);)
MMF(mm_psubsb, for (int i = 0; i < 8; i++) r.sb[i] = mm_s8(a.sb[i] - b.sb[i]);)
MMF(mm_psubsw, for (int i = 0; i < 4; i++) r.sw[i] = mm_s16(a.sw[i] - b.sw[i]);)
MMF(mm_paddusb, for (int i = 0; i < 8; i++) r.b[i] = mm_u8(a.b[i] + b.b[i]);)
MMF(mm_paddusw, for (int i = 0; i < 4; i++) r.w[i] = mm_u16(a.w[i] + b.w[i]);)
MMF(mm_psubusb, for (int i = 0; i < 8; i++) r.b[i] = mm_u8(a.b[i] - b.b[i]);)
MMF(mm_psubusw, for (int i = 0; i < 4; i++) r.w[i] = mm_u16(a.w[i] - b.w[i]);)
MMF(mm_pmullw, for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(a.sw[i] * b.sw[i]);)
MMF(mm_pmulhw, for (int i = 0; i < 4; i++) r.sw[i] = (int16_t)((a.sw[i] * b.sw[i]) >> 16);)
MMF(mm_pmulhuw, for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16);)
MMF(mm_pmaddwd, for (int i = 0; i < 2; i++) r.sd[i] = a.sw[2 * i] * b.sw[2 * i] + a.sw[2 * i + 1] * b.sw[2 * i + 1];)
MMF(mm_pcmpeqb, for (int i = 0; i < 8; i++) r.b[i] = a.b[i] == b.b[i] ? 0xff : 0;)
MMF(mm_pcmpeqw, for (int i = 0; i < 4; i++) r.w[i] = a.w[i] == b.w[i] ? 0xffff : 0;)
MMF(mm_pcmpeqd, for (int i = 0; i < 2; i++) r.d[i] = a.d[i] == b.d[i] ? 0xffffffffu : 0;)
MMF(mm_pcmpgtb, for (int i = 0; i < 8; i++) r.b[i] = a.sb[i] > b.sb[i] ? 0xff : 0;)
MMF(mm_pcmpgtw, for (int i = 0; i < 4; i++) r.w[i] = a.sw[i] > b.sw[i] ? 0xffff : 0;)
MMF(mm_pcmpgtd, for (int i = 0; i < 2; i++) r.d[i] = a.sd[i] > b.sd[i] ? 0xffffffffu : 0;)
MMF(mm_punpcklbw, for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; })
MMF(mm_punpckhbw, for (int i = 0; i < 4; i++) { r.b[2 * i] = a.b[4 + i]; r.b[2 * i + 1] = b.b[4 + i]; })
MMF(mm_punpcklwd, for (int i = 0; i < 2; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; })
MMF(mm_punpckhwd, for (int i = 0; i < 2; i++) { r.w[2 * i] = a.w[2 + i]; r.w[2 * i + 1] = b.w[2 + i]; })
MMF(mm_punpckldq, r.d[0] = a.d[0]; r.d[1] = b.d[0];)
MMF(mm_punpckhdq, r.d[0] = a.d[1]; r.d[1] = b.d[1];)
MMF(mm_packsswb, for (int i = 0; i < 4; i++) { r.sb[i] = mm_s8(a.sw[i]); r.sb[4 + i] = mm_s8(b.sw[i]); })
MMF(mm_packuswb, for (int i = 0; i < 4; i++) { r.b[i] = mm_u8(a.sw[i]); r.b[4 + i] = mm_u8(b.sw[i]); })
MMF(mm_packssdw, for (int i = 0; i < 2; i++) { r.sw[i] = mm_s16(a.sd[i]); r.sw[2 + i] = mm_s16(b.sd[i]); })
MMF(mm_psllw, if (b_ < 16) for (int i = 0; i < 4; i++) r.w[i] = a.w[i] << b_;)
MMF(mm_pslld, if (b_ < 32) for (int i = 0; i < 2; i++) r.d[i] = a.d[i] << b_;)
MMF(mm_psllq, if (b_ < 64) r.q = a.q << b_;)
MMF(mm_psrlw, if (b_ < 16) for (int i = 0; i < 4; i++) r.w[i] = a.w[i] >> b_;)
MMF(mm_psrld, if (b_ < 32) for (int i = 0; i < 2; i++) r.d[i] = a.d[i] >> b_;)
MMF(mm_psrlq, if (b_ < 64) r.q = a.q >> b_;)
MMF(mm_psraw, int n = b_ > 15 ? 15 : (int)b_; for (int i = 0; i < 4; i++) r.sw[i] = a.sw[i] >> n;)
MMF(mm_psrad, int n = b_ > 31 ? 31 : (int)b_; for (int i = 0; i < 2; i++) r.sd[i] = a.sd[i] >> n;)
MMF(mm_pmaxsw, for (int i = 0; i < 4; i++) r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i];)
MMF(mm_pminsw, for (int i = 0; i < 4; i++) r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i];)
MMF(mm_pmaxub, for (int i = 0; i < 8; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i];)
MMF(mm_pminub, for (int i = 0; i < 8; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i];)
MMF(mm_pavgb, for (int i = 0; i < 8; i++) r.b[i] = (a.b[i] + b.b[i] + 1) >> 1;)
MMF(mm_pavgw, for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] + b.w[i] + 1) >> 1;)
MMF(mm_psadbw, int s = 0; for (int i = 0; i < 8; i++) s += a.b[i] > b.b[i] ? a.b[i] - b.b[i] : b.b[i] - a.b[i]; r.q = (uint32_t)s;)

// ---- SSE2 integer helpers that cross the two 64-bit halves (the lane-wise ones reuse the MMX functions per half)
#define XF(name, body) static inline X128 name(X128 a, X128 b) { X128 r; r.q[0] = r.q[1] = 0; body; return r; }
XF(xmm_punpcklbw, for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; })
XF(xmm_punpckhbw, for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[8 + i]; r.b[2 * i + 1] = b.b[8 + i]; })
XF(xmm_punpcklwd, for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; })
XF(xmm_punpckhwd, for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[4 + i]; r.w[2 * i + 1] = b.w[4 + i]; })
XF(xmm_punpckldq, for (int i = 0; i < 2; i++) { r.u[2 * i] = a.u[i]; r.u[2 * i + 1] = b.u[i]; })
XF(xmm_punpckhdq, for (int i = 0; i < 2; i++) { r.u[2 * i] = a.u[2 + i]; r.u[2 * i + 1] = b.u[2 + i]; })
XF(xmm_punpcklqdq, r.q[0] = a.q[0]; r.q[1] = b.q[0];)
XF(xmm_punpckhqdq, r.q[0] = a.q[1]; r.q[1] = b.q[1];)
XF(xmm_packsswb, for (int i = 0; i < 8; i++) { r.b[i] = (uint8_t)mm_s8((int16_t)a.w[i]); r.b[8 + i] = (uint8_t)mm_s8((int16_t)b.w[i]); })
XF(xmm_packuswb, for (int i = 0; i < 8; i++) { r.b[i] = mm_u8((int16_t)a.w[i]); r.b[8 + i] = mm_u8((int16_t)b.w[i]); })
XF(xmm_packssdw, for (int i = 0; i < 4; i++) { r.w[i] = (uint16_t)mm_s16((int32_t)a.u[i]); r.w[4 + i] = (uint16_t)mm_s16((int32_t)b.u[i]); })
static inline X128 xmm_pslldq(X128 a, unsigned n) { X128 r; r.q[0] = r.q[1] = 0; if (n > 15) n = 16; for (unsigned i = n; i < 16; i++) r.b[i] = a.b[i - n]; return r; }
static inline X128 xmm_psrldq(X128 a, unsigned n) { X128 r; r.q[0] = r.q[1] = 0; if (n > 15) n = 16; for (unsigned i = 0; i + n < 16; i++) r.b[i] = a.b[i + n]; return r; }
