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

// NEON versions of the hot MMX operations (video decoding, IDCT): same results as the scalar helpers above
#if defined(__aarch64__)
#include <arm_neon.h>
#define N_U16(x) vreinterpret_u16_u64(vcreate_u64(x))
#define N_S16(x) vreinterpret_s16_u64(vcreate_u64(x))
#define N_U8(x) vreinterpret_u8_u64(vcreate_u64(x))
#define N_S8(x) vreinterpret_s8_u64(vcreate_u64(x))
#define N_S32(x) vreinterpret_s32_u64(vcreate_u64(x))
#define R_U16(v) vget_lane_u64(vreinterpret_u64_u16(v), 0)
#define R_S16(v) vget_lane_u64(vreinterpret_u64_s16(v), 0)
#define R_U8(v) vget_lane_u64(vreinterpret_u64_u8(v), 0)
#define R_S8(v) vget_lane_u64(vreinterpret_u64_s8(v), 0)
#define R_S32(v) vget_lane_u64(vreinterpret_u64_s32(v), 0)
#define NF(name, expr) static inline uint64_t name(uint64_t a_, uint64_t b_) { return expr; }
NF(mmn_paddb, R_U8(vadd_u8(N_U8(a_), N_U8(b_))))
NF(mmn_paddw, R_U16(vadd_u16(N_U16(a_), N_U16(b_))))
NF(mmn_psubb, R_U8(vsub_u8(N_U8(a_), N_U8(b_))))
NF(mmn_psubw, R_U16(vsub_u16(N_U16(a_), N_U16(b_))))
NF(mmn_paddsb, R_S8(vqadd_s8(N_S8(a_), N_S8(b_))))
NF(mmn_paddsw, R_S16(vqadd_s16(N_S16(a_), N_S16(b_))))
NF(mmn_psubsb, R_S8(vqsub_s8(N_S8(a_), N_S8(b_))))
NF(mmn_psubsw, R_S16(vqsub_s16(N_S16(a_), N_S16(b_))))
NF(mmn_paddusb, R_U8(vqadd_u8(N_U8(a_), N_U8(b_))))
NF(mmn_paddusw, R_U16(vqadd_u16(N_U16(a_), N_U16(b_))))
NF(mmn_psubusb, R_U8(vqsub_u8(N_U8(a_), N_U8(b_))))
NF(mmn_psubusw, R_U16(vqsub_u16(N_U16(a_), N_U16(b_))))
NF(mmn_pmullw, R_S16(vmul_s16(N_S16(a_), N_S16(b_))))
NF(mmn_pmulhw, R_S16(vshrn_n_s32(vmull_s16(N_S16(a_), N_S16(b_)), 16)))
NF(mmn_pmulhuw, R_U16(vshrn_n_u32(vmull_u16(N_U16(a_), N_U16(b_)), 16)))
static inline uint64_t mmn_pmaddwd(uint64_t a_, uint64_t b_) { int32x4_t p = vmull_s16(N_S16(a_), N_S16(b_)); return R_S32(vpadd_s32(vget_low_s32(p), vget_high_s32(p))); }
NF(mmn_punpcklbw, R_U8(vzip1_u8(N_U8(a_), N_U8(b_))))
NF(mmn_punpckhbw, R_U8(vzip2_u8(N_U8(a_), N_U8(b_))))
NF(mmn_punpcklwd, R_U16(vzip1_u16(N_U16(a_), N_U16(b_))))
NF(mmn_punpckhwd, R_U16(vzip2_u16(N_U16(a_), N_U16(b_))))
NF(mmn_packuswb, R_U8(vqmovun_s16(vcombine_s16(N_S16(a_), N_S16(b_)))))
NF(mmn_packsswb, R_S8(vqmovn_s16(vcombine_s16(N_S16(a_), N_S16(b_)))))
NF(mmn_packssdw, R_S16(vqmovn_s32(vcombine_s32(N_S32(a_), N_S32(b_)))))
static inline uint64_t mmn_psllw(uint64_t a_, uint64_t b_) { return b_ < 16 ? R_U16(vshl_u16(N_U16(a_), vdup_n_s16((int16_t)b_))) : 0; }
static inline uint64_t mmn_psrlw(uint64_t a_, uint64_t b_) { return b_ < 16 ? R_U16(vshl_u16(N_U16(a_), vdup_n_s16((int16_t)-(int)b_))) : 0; }
static inline uint64_t mmn_psraw(uint64_t a_, uint64_t b_) { int n = b_ > 15 ? 15 : (int)b_; return R_S16(vshl_s16(N_S16(a_), vdup_n_s16((int16_t)-n))); }
NF(mmn_pmaxsw, R_S16(vmax_s16(N_S16(a_), N_S16(b_))))
NF(mmn_pminsw, R_S16(vmin_s16(N_S16(a_), N_S16(b_))))
NF(mmn_pmaxub, R_U8(vmax_u8(N_U8(a_), N_U8(b_))))
NF(mmn_pminub, R_U8(vmin_u8(N_U8(a_), N_U8(b_))))
NF(mmn_pavgb, R_U8(vrhadd_u8(N_U8(a_), N_U8(b_))))
NF(mmn_pavgw, R_U16(vrhadd_u16(N_U16(a_), N_U16(b_))))
#define mm_paddb mmn_paddb
#define mm_paddw mmn_paddw
#define mm_psubb mmn_psubb
#define mm_psubw mmn_psubw
#define mm_paddsb mmn_paddsb
#define mm_paddsw mmn_paddsw
#define mm_psubsb mmn_psubsb
#define mm_psubsw mmn_psubsw
#define mm_paddusb mmn_paddusb
#define mm_paddusw mmn_paddusw
#define mm_psubusb mmn_psubusb
#define mm_psubusw mmn_psubusw
#define mm_pmullw mmn_pmullw
#define mm_pmulhw mmn_pmulhw
#define mm_pmulhuw mmn_pmulhuw
#define mm_pmaddwd mmn_pmaddwd
#define mm_punpcklbw mmn_punpcklbw
#define mm_punpckhbw mmn_punpckhbw
#define mm_punpcklwd mmn_punpcklwd
#define mm_punpckhwd mmn_punpckhwd
#define mm_packuswb mmn_packuswb
#define mm_packsswb mmn_packsswb
#define mm_packssdw mmn_packssdw
#define mm_psllw mmn_psllw
#define mm_psrlw mmn_psrlw
#define mm_psraw mmn_psraw
#define mm_pmaxsw mmn_pmaxsw
#define mm_pminsw mmn_pminsw
#define mm_pmaxub mmn_pmaxub
#define mm_pminub mmn_pminub
#define mm_pavgb mmn_pavgb
#define mm_pavgw mmn_pavgw
#endif

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
