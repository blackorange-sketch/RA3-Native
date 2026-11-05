// Runtime helpers for lifted x87 and SSE instructions.
#pragma once
#include <cmath>
#include <cstring>

#include "cpu.h"

// ---- memory
static inline float ldf32(uint32_t a) { return guest::load<float>(a); }
static inline double ldf64(uint32_t a) { return guest::load<double>(a); }
static inline void stf32(uint32_t a, float v) { guest::store<float>(a, v); }
static inline void stf64(uint32_t a, double v) { guest::store<double>(a, v); }
static inline X128 ld128(uint32_t a) { return guest::load<X128>(a); }
static inline void st128(uint32_t a, const X128& v) { guest::store<X128>(a, v); }

// ---- x87
static inline double& ST(CPU* c, int i) { return c->st[(c->top + i) & 7]; }
static inline void fpush(CPU* c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double fpop(CPU* c) { double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }

// Precision control: 24-bit mantissa rounds every result to float, which is exact for + - * / sqrt.
static inline double fround(CPU* c, double x) { return ((c->fcw >> 8) & 3) == 0 ? (double)(float)x : x; }

enum { FOP_ADD, FOP_SUB, FOP_MUL, FOP_DIV };
static inline double fapply(int op, double a, double b)
{
	switch (op) { case FOP_ADD: return a + b; case FOP_SUB: return a - b; case FOP_MUL: return a * b; default: return a / b; }
}

// dst = a op b (or b op a when rev); then optionally pop.
static inline void farith(CPU* c, int op, int dst, double other, bool rev, bool pop)
{
	double a = ST(c, dst);
	double r = rev ? fapply(op, other, a) : fapply(op, a, other);
	ST(c, dst) = fround(c, r);
	if (pop) fpop(c);
}

static inline int64_t fcvt_mode(double x, int bits, int mode)
{
	double r;
	switch (mode)
	{
	case 0: r = std::nearbyint(x); break;
	case 1: r = std::floor(x); break;
	case 2: r = std::ceil(x); break;
	default: r = std::trunc(x); break;
	}
	double lim = std::ldexp(1.0, bits - 1);
	if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : bits == 32 ? (int64_t)INT32_MIN : (int64_t)INT16_MIN;
	return (int64_t)r;
}
static inline int64_t fcvt(CPU* c, double x, int bits) { return fcvt_mode(x, bits, (c->fcw >> 10) & 3); }

static inline void fsetcc(CPU* c, double a, double b)
{
	if (std::isnan(a) || std::isnan(b)) c->fcc = 0x4500;
	else if (a > b) c->fcc = 0;
	else if (a < b) c->fcc = 0x0100;
	else c->fcc = 0x4000;
}
// fcomi: returns CF/ZF/PF as bits 0/6/2 of a flags byte (same layout as lahf)
static inline unsigned fcmpflags(double a, double b)
{
	if (std::isnan(a) || std::isnan(b)) return 1 | 4 | 0x40;
	if (a > b) return 0;
	if (a < b) return 1;
	return 0x40;
}
static inline uint16_t fstatus(CPU* c) { return (uint16_t)(c->fcc | (c->top << 11)); }

// ---- SSE
static inline float sse_min(float a, float b) { return a < b ? a : b; }
static inline float sse_max(float a, float b) { return a > b ? a : b; }
static inline int32_t sse_cvtt(float x) { return (x >= -2147483648.0f && x < 2147483648.0f) ? (int32_t)x : INT32_MIN; }
static inline int32_t sse_cvt(float x) { float r = std::nearbyintf(x); return (r >= -2147483648.0f && r < 2147483648.0f) ? (int32_t)r : INT32_MIN; }
static inline uint32_t sse_cmp(int pred, float a, float b)
{
	bool un = std::isnan(a) || std::isnan(b), r;
	switch (pred)
	{
	case 0: r = a == b; break;
	case 1: r = a < b; break;
	case 2: r = a <= b; break;
	case 3: r = un; break;
	case 4: r = !(a == b); break;
	case 5: r = !(a < b); break;
	case 6: r = !(a <= b); break;
	default: r = !un; break;
	}
	return r ? 0xffffffffu : 0;
}

// ---- misc
static inline void cpuid_impl(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
{
	uint32_t leaf = a;
	a = b = c = d = 0;
	if (leaf == 0) { a = 1; b = 0x756e6547; d = 0x49656e69; c = 0x6c65746e; }   // "GenuineIntel"
	else if (leaf == 1) { a = 0x00000f00; d = 1u | (1u << 4) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25); }   // FPU TSC CMOV MMX FXSR SSE
}
