// Parser for compiled Direct3D 9 effects (D3DX "FX 9" binary, tag 0xfeff0901): parameters, techniques, passes and
// their annotations (names, types, simple values). Shader byte code / state values are not interpreted yet.
// Everything is bounds-checked; a malformed blob simply fails to parse.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace d3d
{
struct FxAnn
{
	std::string name, str;
	uint32_t type = 0, cls = 0, rows = 1, cols = 1, elements = 0;
	std::vector<uint32_t> vals;   // raw 32-bit values (numeric annotations)
};
struct FxParam
{
	std::string name, semantic;
	uint32_t type = 0, cls = 0, rows = 1, cols = 1, elements = 0, flags = 0, members = 0;
	std::vector<FxAnn> anns;
};
struct FxPass
{
	std::string name;
	std::vector<FxAnn> anns;
	uint32_t states = 0;
};
struct FxTech
{
	std::string name;
	std::vector<FxAnn> anns;
	std::vector<FxPass> passes;
};
struct FxInfo
{
	std::vector<FxParam> params;
	std::vector<FxTech> techs;
	uint32_t current = 0;
	uint32_t base = 0;   // header size that offsets are relative to (4 or 8)
};

namespace fxparse
{
struct Rd
{
	const uint8_t* d;
	size_t n;
	bool ok = true;
	uint32_t u32(size_t& p)
	{
		if (p + 4 > n) { ok = false; return 0; }
		uint32_t v;
		memcpy(&v, d + p, 4);
		p += 4;
		return v;
	}
	std::string str(size_t base, uint32_t off)   // length-prefixed (size includes the terminator)
	{
		if (!off) return "";
		size_t p = base + off;
		uint32_t len = u32(p);
		if (!ok || len > 4096 || p + len > n) { ok = false; return ""; }
		std::string s((const char*)d + p, len);
		while (!s.empty() && s.back() == 0) s.pop_back();
		for (unsigned char ch : s) if (ch < 32 || ch > 126) { ok = false; return ""; }
		return s;
	}
};

// Typedef at `off` (relative to base). Fills type/class/name/semantic/size info.
inline bool typedefAt(Rd& r, size_t base, uint32_t off, FxParam& out, int depth = 0)
{
	if (depth > 4) return false;
	size_t p = base + off;
	out.type = r.u32(p);
	out.cls = r.u32(p);
	uint32_t nameOff = r.u32(p), semOff = r.u32(p);
	out.elements = r.u32(p);
	if (!r.ok || out.type > 31 || out.cls > 8 || out.elements > 4096) return false;
	out.name = r.str(base, nameOff);
	out.semantic = r.str(base, semOff);
	if (!r.ok) return false;
	switch (out.cls)
	{
	case 0: case 1: case 2: case 3:
		out.cols = r.u32(p);
		out.rows = r.u32(p);
		if (out.cols > 16 || out.rows > 16) return false;
		break;
	case 5:
	{
		out.members = r.u32(p);
		if (out.members > 256) return false;
		for (uint32_t i = 0; i < out.members; i++)
		{
			// members are inline typedefs; parse and discard (their size is needed to continue)
			FxParam m;
			uint32_t mt = r.u32(p), mc = r.u32(p), mn = r.u32(p), ms = r.u32(p), me = r.u32(p);
			(void)mn; (void)ms; (void)me;
			if (mc <= 3) { r.u32(p); r.u32(p); }
			else if (mc == 5) return true;   // nested structs: give up on exact layout
			(void)mt;
		}
		break;
	}
	default: break;
	}
	return r.ok;
}

inline bool annotation(Rd& r, size_t base, size_t& p, FxAnn& a)
{
	uint32_t tdOff = r.u32(p), valOff = r.u32(p);
	FxParam t;
	if (!r.ok || !typedefAt(r, base, tdOff, t)) return false;
	a.name = t.name; a.type = t.type; a.cls = t.cls; a.rows = t.rows; a.cols = t.cols; a.elements = t.elements;
	if (t.type == 4)   // string: value is the offset of a length-prefixed string
	{
		size_t vp = base + valOff;
		uint32_t so = r.u32(vp);
		Rd probe = r;
		std::string s = probe.str(base, so);
		if (probe.ok) a.str = s;
	}
	else if (t.type >= 1 && t.type <= 3 && t.cls <= 3)
	{
		uint32_t cnt = std::max(1u, t.elements) * t.rows * t.cols;
		size_t vp = base + valOff;
		if (cnt <= 64 && vp + 4ull * cnt <= r.n)
			for (uint32_t i = 0; i < cnt; i++) a.vals.push_back(r.u32(vp));
	}
	return true;
}

inline bool tryParse(const uint8_t* d, size_t n, uint32_t hdr, FxInfo& fx)
{
	Rd r{d, n};
	size_t p = 4;
	uint32_t start = r.u32(p);
	size_t base = hdr;
	p = base + start;
	uint32_t nParam = r.u32(p), nTech = r.u32(p);
	r.u32(p);              // unknown
	r.u32(p);              // object count
	if (!r.ok || nParam > 4096 || nTech > 512) return false;
	for (uint32_t i = 0; i < nParam; i++)
	{
		FxParam prm;
		uint32_t tdOff = r.u32(p);
		r.u32(p);                              // default value offset
		prm.flags = r.u32(p);
		uint32_t nAnn = r.u32(p);
		if (!r.ok || nAnn > 256 || !typedefAt(r, base, tdOff, prm)) return false;
		for (uint32_t k = 0; k < nAnn; k++)
		{
			FxAnn a;
			if (!annotation(r, base, p, a)) return false;
			prm.anns.push_back(std::move(a));
		}
		fx.params.push_back(std::move(prm));
	}
	for (uint32_t i = 0; i < nTech; i++)
	{
		FxTech t;
		uint32_t nameOff = r.u32(p), nAnn = r.u32(p), nPass = r.u32(p);
		if (!r.ok || nAnn > 256 || nPass > 64 || !nameOff) return false;
		t.name = r.str(base, nameOff);
		if (!r.ok || t.name.empty()) return false;
		for (uint32_t k = 0; k < nAnn; k++)
		{
			FxAnn a;
			if (!annotation(r, base, p, a)) return false;
			t.anns.push_back(std::move(a));
		}
		for (uint32_t k = 0; k < nPass; k++)
		{
			FxPass ps;
			uint32_t pn = r.u32(p), pa = r.u32(p), st = r.u32(p);
			if (!r.ok || pa > 256 || st > 1024) return false;
			ps.name = r.str(base, pn);
			if (!r.ok) return false;
			ps.states = st;
			for (uint32_t j = 0; j < pa; j++)
			{
				FxAnn a;
				if (!annotation(r, base, p, a)) return false;
				ps.anns.push_back(std::move(a));
			}
			p += 16ull * st;   // states: operation, index, typedef offset, value offset
			if (p > n) return false;
			t.passes.push_back(std::move(ps));
		}
		fx.techs.push_back(std::move(t));
	}
	fx.base = hdr;
	return r.ok;
}
}   // namespace fxparse

inline bool parseEffect(const uint8_t* d, size_t n, FxInfo& fx)
{
	if (n < 16) return false;
	uint32_t tag;
	memcpy(&tag, d, 4);
	if (tag != 0xfeff0901) return false;
	for (uint32_t hdr : {8u, 4u})
	{
		FxInfo t;
		if (fxparse::tryParse(d, n, hdr, t) && !t.techs.empty()) { fx = std::move(t); return true; }
	}
	return false;
}
}   // namespace d3d
