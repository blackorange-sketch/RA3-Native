// Effect runtime helpers that do not touch guest memory or GL: the D3DX state table, parameter value storage and
// register packing, FXLC pre-shader evaluation and technique/pass shader resolution. Unit-tested on the host.
#pragma once
#include <cmath>
#include <deque>
#include <functional>

#include "d3d9_sh.h"
#include "d3dx_fx.h"

namespace d3d
{
namespace fxrt
{
enum StateClass : uint8_t { SC_NONE = 0, SC_RS, SC_TSS, SC_VS, SC_PS, SC_SAMP, SC_TEXTURE, SC_SETSAMPLER, SC_CONST };
struct StateInfo { StateClass cls; uint32_t id; };

inline StateInfo stateInfo(uint32_t op)
{
	static const uint16_t rs[103] = {
		7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60,
		128, 129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205,
		136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 166, 156, 157, 158, 159, 160, 161, 162, 163, 165, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 206, 207, 208, 209};
	if (op < 103) return {SC_RS, rs[op]};
	if (op >= 103 && op <= 120) return {SC_TSS, op - 103 + 1};
	if (op == 146) return {SC_VS, 0};
	if (op == 147) return {SC_PS, 0};
	if (op >= 148 && op <= 163) return {SC_CONST, op};
	if (op == 164) return {SC_TEXTURE, 0};
	static const uint8_t samp[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};   // ADDRESSU,V,W,BORDERCOLOR,MAG,MIN,MIP,BIAS,MAXMIP,MAXANISO,SRGB,ELEMENTINDEX,DMAPOFFSET
	if (op >= 165 && op <= 177) return {SC_SAMP, samp[op - 165]};
	if (op == 178) return {SC_SETSAMPLER, 0};
	return {SC_NONE, 0};
}

// ---- register packing -------------------------------------------------------------------------------------------
inline uint32_t convScalar(uint32_t srcType, uint32_t dstSet /* 2 float, 1 int, 0 bool */, uint32_t v)
{
	float f;
	int32_t i;
	if (srcType == 3) { memcpy(&f, &v, 4); i = (int32_t)f; }
	else { i = (int32_t)v; f = (srcType == 1) ? (v ? 1.f : 0.f) : (float)i; }
	if (dstSet == 2) { memcpy(&v, &f, 4); return v; }
	if (dstSet == 1) return (uint32_t)i;
	return srcType == 3 ? (f != 0.f) : (i != 0);
}

struct Packer
{
	uint32_t* dst;        // 4 uint32 per register (bool set: one uint32 per index)
	uint32_t maxReg;
	uint32_t set;
	uint32_t cursor;      // in components
	const uint32_t* src;
	const uint32_t* srcEnd;
	bool alignReg() { cursor = (cursor + 3) & ~3u; return true; }
	void put(uint32_t srcType, uint32_t comp)
	{
		uint32_t v = (src < srcEnd) ? *src++ : 0;
		v = convScalar(srcType, set, v);
		if (set == 0) { if (comp < maxReg) dst[comp] = v; }
		else if (comp / 4 < maxReg) dst[comp] = v;
	}
	void emit(const FxType& t, bool inArray)
	{
		if (t.cls == 4) return;
		if (t.elements && !inArray)
		{
			for (uint32_t e = 0; e < t.elements; e++)
			{
				if (set != 0) alignReg();
				FxType one = t;
				one.elements = 0;
				emit(one, true);
				if (set != 0) alignReg();
			}
			return;
		}
		switch (t.cls)
		{
		case 0:
			if (set == 0) { put(t.type, cursor); cursor++; }
			else { put(t.type, cursor); cursor++; }
			break;
		case 1:
			if (set != 0 && (cursor & 3) + t.cols > 4) alignReg();
			for (uint32_t c = 0; c < t.cols; c++) { put(t.type, cursor); cursor++; }
			break;
		case 2: case 3:
		{
			uint32_t nreg = t.cls == 3 ? t.rows : t.cols, inner = t.cls == 3 ? t.cols : t.rows;   // the binary's class 2 is the default column-major layout: registers hold columns
			if (set != 0) alignReg();
			for (uint32_t r = 0; r < nreg; r++)
			{
				for (uint32_t c = 0; c < inner; c++) put(t.type, cursor + c);
				if (set != 0) cursor += 4; else cursor += inner;
			}
			break;
		}
		case 5:
			if (set != 0) alignReg();
			for (auto& m : t.members) emit(m, false);
			if (set != 0) alignReg();
			break;
		}
	}
};

// Writes the packed value of a parameter (or sub-parameter) into registers starting at `startReg`.
inline void packToRegs(const FxType& t, const uint32_t* src, size_t nsrc, uint32_t set, uint32_t startReg, uint32_t* dst, uint32_t maxReg)
{
	Packer p{dst, maxReg, set, set == 0 ? startReg : startReg * 4, src, src + nsrc};
	p.emit(t, false);
}

// ---- parameters ---------------------------------------------------------------------------------------------------
inline uint32_t paramScalars(const FxParam& p) { return p.ty.scalars() * std::max(1u, p.ty.elements); }

inline void initParams(FxInfo& fx)
{
	for (auto& p : fx.params)
	{
		if (p.ty.cls == 4) continue;
		uint32_t n = paramScalars(p);
		p.val.assign(n, 0);
		size_t off = fx.base + p.dv;
		if (p.dv && off + 4ull * n <= fx.blob.size()) memcpy(p.val.data(), fx.blob.data() + off, 4ull * n);
	}
	// sampler parameters: states stored at the default value: count, then {op, index, typedef offset, value offset}
	for (size_t i = 0; i < fx.params.size(); i++)
	{
		FxParam& p = fx.params[i];
		if (!(p.ty.cls == 4 && p.ty.type >= 10 && p.ty.type <= 14) || !p.dv) continue;
		size_t q = fx.base + p.dv;
		uint32_t cnt = fx.u32(q);
		q += 4;
		if (cnt > 32) continue;
		for (uint32_t k = 0; k < cnt; k++, q += 16)
		{
			uint32_t op = fx.u32(q), vo = fx.u32(q + 12);
			StateInfo si = stateInfo(op);
			if (si.cls == SC_SAMP) p.smp.push_back({si.id, fx.u32(fx.base + vo)});
		}
	}
	// sampler -> texture parameter links come from the parameter-reference resource records (tech 0xffffffff)
	for (auto& r : fx.res)
	{
		if (r.tech != 0xffffffffu || r.usage != 1 || r.pass >= fx.params.size()) continue;
		std::string name((const char*)fx.blob.data() + r.off, strnlen((const char*)fx.blob.data() + r.off, r.size));
		for (size_t j = 0; j < fx.params.size(); j++)
			if (fx.params[j].name == name) { fx.params[r.pass].smpTex = (uint32_t)j + 1; break; }
	}
}

inline int findParam(const FxInfo& fx, const std::string& name)
{
	for (size_t i = 0; i < fx.params.size(); i++) if (fx.params[i].name == name) return (int)i;
	return -1;
}

// ---- FXLC pre-shader evaluation -------------------------------------------------------------------------------------
// `getInputs` fills the input register file (4 doubles per register) for the CTAB constants.
inline bool evalPreshader(const uint8_t* blob, size_t n, const std::function<void(const d3d::CtabConst&, double*, size_t)>& fill, double& result)
{
	auto u = [&](size_t p) { uint32_t v = 0; if (p + 4 <= n) memcpy(&v, blob + p, 4); return v; };
	if (n < 8 || u(0) != 0x46580200) return false;
	size_t p = 4;
	const uint8_t* ctabP = nullptr; size_t ctabN = 0;
	const uint8_t* clitP = nullptr; size_t clitN = 0;
	const uint8_t* fxlcP = nullptr; size_t fxlcN = 0;
	while (p + 4 <= n)
	{
		uint32_t t = u(p);
		if (t == 0xffff) break;
		if ((t & 0xffff) != 0xfffe) return false;
		size_t len = (size_t)(t >> 16) * 4;
		if (p + 4 + len > n) return false;
		uint32_t tag = u(p + 4);
		const uint8_t* body = blob + p + 8;
		size_t bl = len >= 4 ? len - 4 : 0;
		if (tag == 0x42415443) { ctabP = body; ctabN = bl; }
		else if (tag == 0x54494c43) { clitP = body; clitN = bl; }
		else if (tag == 0x434c5846) { fxlcP = body; fxlcN = bl; }
		p += 4 + len;
	}
	if (!fxlcP) return false;
	std::vector<d3d::CtabConst> ctab;
	if (ctabP) shdr::parseCtab(ctabP, ctabN, ctab);
	std::vector<double> in(1024, 0.0), tmp(256, 0.0), lit;
	for (auto& c : ctab)
	{
		if (c.regIndex * 4 + 4 * std::max(1u, c.regCount) <= in.size()) fill(c, &in[c.regIndex * 4], 4 * std::max(1u, c.regCount));
	}
	if (clitP && clitN >= 4)
	{
		uint32_t cnt;
		memcpy(&cnt, clitP, 4);
		if (4 + 8ull * cnt <= clitN) { lit.resize(cnt); memcpy(lit.data(), clitP + 4, 8ull * cnt); }
	}
	auto fu = [&](size_t q) { uint32_t v = 0; if (q + 4 <= fxlcN) memcpy(&v, fxlcP + q, 4); return v; };
	uint32_t cnt = fu(0);
	size_t q = 4;
	double out = 0;
	auto rd = [&](uint32_t type, uint32_t idx) -> double {
		switch (type)
		{
		case 1: return idx < lit.size() ? lit[idx] : 0.0;
		case 2: return idx < in.size() ? in[idx] : 0.0;
		case 7: return idx < tmp.size() ? tmp[idx] : 0.0;
		case 4: return out;
		}
		return 0.0;
	};
	for (uint32_t i = 0; i < cnt && cnt < 4096; i++)
	{
		uint32_t tok = fu(q), nin = fu(q + 4);
		q += 8;
		if (nin > 8) return false;
		double a[8] = {};
		for (uint32_t k = 0; k < nin; k++) { a[k] = rd(fu(q + 4), fu(q + 8)); q += 12; }
		uint32_t ot = fu(q + 4), oi = fu(q + 8);
		q += 12;
		uint32_t opc = (tok >> 20) & 0x7ff;
		double r = 0;
		switch (opc)
		{
		case 0x100: r = a[0]; break;
		case 0x101: r = -a[0]; break;
		case 0x102: r = a[0] != 0 ? 1.0 / a[0] : 0.0; break;
		case 0x103: r = a[0] - std::floor(a[0]); break;
		case 0x104: r = std::exp2(a[0]); break;
		case 0x105: r = std::log2(a[0]); break;
		case 0x106: r = a[0] > 0 ? 1.0 / std::sqrt(a[0]) : 0.0; break;
		case 0x107: r = std::sin(a[0]); break;
		case 0x108: r = std::cos(a[0]); break;
		case 0x200: r = std::min(a[0], a[1]); break;
		case 0x201: r = std::max(a[0], a[1]); break;
		case 0x202: r = a[0] < a[1] ? 1.0 : 0.0; break;
		case 0x203: r = a[0] >= a[1] ? 1.0 : 0.0; break;
		case 0x204: r = a[0] + a[1]; break;
		case 0x205: r = a[0] * a[1]; break;
		case 0x206: r = std::atan2(a[0], a[1]); break;
		case 0x207: r = a[1] != 0 ? a[0] / a[1] : 0.0; break;
		case 0x300: r = a[0] >= 0 ? a[1] : a[2]; break;
		default: return false;
		}
		if (ot == 4) out = r;
		else if (ot == 7 && oi < tmp.size()) tmp[oi] = r;
	}
	result = out;
	return true;
}

struct ShaderRef { const uint8_t* code = nullptr; size_t size = 0; int arrayIndex = -1; std::string arrayName; };

// Finds the pre-shader/array machinery of a shader state of a pass. `valueOf` provides parameter values (as registers).
inline bool resolveShader(const FxInfo& fx, uint32_t tech, uint32_t pass, uint32_t stateIdx, ShaderRef& out)
{
	for (auto& r : fx.res)
	{
		if (r.tech != tech || r.pass != pass || r.state != stateIdx) continue;
		const uint8_t* d = fx.blob.data() + r.off;
		if (r.usage == 0) { out.code = d; out.size = r.size; return r.size >= 8; }
		if (r.usage == 1)   // shader array element named directly: "PS_Array[0]"
		{
			std::string name((const char*)d, strnlen((const char*)d, r.size));
			int idx = 0;
			size_t br = name.find('[');
			if (br != std::string::npos) { idx = atoi(name.c_str() + br + 1); name.resize(br); }
			int pi = findParam(fx, name);
			if (pi < 0) return false;
			const FxParam& ap = fx.params[pi];
			if (ap.ty.cls != 4 || (uint32_t)idx >= std::max(1u, ap.ty.elements)) return false;
			uint32_t id = fx.u32(fx.base + ap.dv + 4 * idx);
			auto it = fx.objs.find(id);
			if (it == fx.objs.end()) return false;
			out.code = fx.blob.data() + it->second.first;
			out.size = it->second.second;
			out.arrayIndex = idx;
			out.arrayName = name;
			return out.size >= 8;
		}
		if (r.usage == 2)
		{
			if (r.size < 8) return false;
			uint32_t nl;
			memcpy(&nl, d, 4);
			if (4ull + nl > r.size) return false;
			std::string name((const char*)d + 4, strnlen((const char*)d + 4, nl));
			int pi = findParam(fx, name);
			if (pi < 0) return false;
			const FxParam& ap = fx.params[pi];
			std::function<void(const d3d::CtabConst&, double*, size_t)> fill = [&](const d3d::CtabConst& c, double* dst, size_t nd) {
				int k = findParam(fx, c.name);
				if (k < 0) return;
				const FxParam& pp = fx.params[k];
				std::vector<uint32_t> regs(nd, 0);
				packToRegs(pp.ty, pp.val.data(), pp.val.size(), 2, 0, regs.data(), (uint32_t)(nd / 4));
				for (size_t i = 0; i < nd; i++) { float f; memcpy(&f, &regs[i], 4); dst[i] = f; }
			};
			double res = 0;
			if (!evalPreshader(d + 4 + nl, r.size - 4 - nl, fill, res)) return false;
			int idx = (int)res;
			uint32_t n = std::max(1u, ap.ty.elements);
			if (idx < 0 || (uint32_t)idx >= n) idx = 0;
			uint32_t id = fx.u32(fx.base + ap.dv + 4 * idx);
			auto it = fx.objs.find(id);
			if (it == fx.objs.end()) return false;
			out.code = fx.blob.data() + it->second.first;
			out.size = it->second.second;
			out.arrayIndex = idx;
			out.arrayName = name;
			return out.size >= 8;
		}
	}
	return false;
}
}   // namespace fxrt
}   // namespace d3d
