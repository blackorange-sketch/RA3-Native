// Parser for compiled Direct3D 9 effects (D3DX "FX 9" binary, tag 0xfeff0901): parameters (with full types), techniques,
// passes with their states, the string/shader object table and the resource records (shaders, array selectors,
// parameter references). Everything is bounds-checked; a malformed blob simply fails to parse.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace d3d
{
struct FxAnn
{
	std::string name, str;
	uint32_t type = 0, cls = 0, rows = 1, cols = 1, elements = 0;
	std::vector<uint32_t> vals;   // raw 32-bit values (numeric annotations)
	uint32_t objId = 0xffffffffu;   // string annotations: id of the string object holding the value
};
// D3DXPARAMETER_CLASS: 0 scalar, 1 vector, 2 matrix rows, 3 matrix columns, 4 object, 5 struct
// D3DXPARAMETER_TYPE: 0 void, 1 bool, 2 int, 3 float, 4 string, 5 texture, 6 texture1d, 7 texture2d, 8 texture3d, 9 texturecube,
//                     10 sampler, 11 sampler1d, 12 sampler2d, 13 sampler3d, 14 samplercube, 15 pixelshader, 16 vertexshader
struct FxType
{
	uint32_t type = 0, cls = 0, rows = 1, cols = 1, elements = 0;
	std::string name, semantic;
	std::vector<FxType> members;
	uint32_t scalars() const   // scalar count of one element
	{
		if (cls == 5) { uint32_t n = 0; for (auto& m : members) n += m.scalars() * std::max(1u, m.elements); return n; }
		if (cls == 4) return 1;
		return rows * cols;
	}
};
struct FxParam
{
	std::string name, semantic;
	uint32_t type = 0, cls = 0, rows = 1, cols = 1, elements = 0, flags = 0, members = 0;
	std::vector<FxAnn> anns;
	FxType ty;
	uint32_t dv = 0;          // default value offset (relative to base)
	std::string str;          // string parameters: default value (resolved from the object table)
	std::vector<uint32_t> val;   // current value, tightly packed (4 bytes per scalar); object params hold ids / unused
	uint32_t texObj = 0;      // texture parameters: guest address of the bound texture object
	uint32_t smpTex = 0;      // sampler parameters: index of the texture parameter it samples + 1
	struct SS { uint32_t op, v; };
	std::vector<SS> smp;      // sampler parameters: states (D3DSAMP_* id, value)
};
struct FxState { uint32_t op = 0, index = 0, vo = 0, typeCls = 0, typeType = 0; };
struct FxPass
{
	std::string name;
	std::vector<FxAnn> anns;
	uint32_t states = 0;
	std::vector<FxState> sts;
};
struct FxTech
{
	std::string name;
	std::vector<FxAnn> anns;
	std::vector<FxPass> passes;
};
struct FxRes   // a state value that is not a plain constant
{
	uint32_t tech = 0, pass = 0, el = 0, state = 0, usage = 0;   // usage: 0 shader/constant data, 1 parameter reference, 2 array selector
	uint32_t off = 0, size = 0;                                  // data (relative to blob start)
};
struct FxInfo
{
	std::vector<FxParam> params;
	std::vector<FxTech> techs;
	uint32_t current = 0;
	uint32_t base = 0;   // header size that offsets are relative to (4 or 8)
	std::vector<uint8_t> blob;
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> objs;   // object id -> (offset, size) of its data in blob
	std::vector<FxRes> res;
	uint32_t u32(size_t p) const { uint32_t v = 0; if (p + 4 <= blob.size()) memcpy(&v, blob.data() + p, 4); return v; }
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

// Typedef at stream position p (members follow inline).
inline bool typedefAtPos(Rd& r, size_t base, size_t& p, FxType& out, int depth = 0)
{
	if (depth > 6) return false;
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
	case 0: out.rows = out.cols = 1; r.u32(p); r.u32(p); break;
	case 1: out.cols = r.u32(p); out.rows = r.u32(p); if (out.rows > 16 || out.cols > 16) return false; break;
	case 2: case 3: out.rows = r.u32(p); out.cols = r.u32(p); if (out.rows > 16 || out.cols > 16) return false; break;
	case 5:
	{
		uint32_t m = r.u32(p);
		if (m > 256) return false;
		out.members.resize(m);
		for (uint32_t i = 0; i < m; i++)
			if (!typedefAtPos(r, base, p, out.members[i], depth + 1)) return false;
		break;
	}
	default: break;
	}
	return r.ok;
}
inline bool typedefAt(Rd& r, size_t base, uint32_t off, FxType& out)
{
	size_t p = base + off;
	return typedefAtPos(r, base, p, out);
}

inline bool annotation(Rd& r, size_t base, size_t& p, FxAnn& a)
{
	uint32_t tdOff = r.u32(p), valOff = r.u32(p);
	FxType t;
	if (!r.ok || !typedefAt(r, base, tdOff, t)) return false;
	a.name = t.name; a.type = t.type; a.cls = t.cls; a.rows = t.rows; a.cols = t.cols; a.elements = t.elements;
	if (t.type == 4)   // string: value is the offset of a length-prefixed string
	{
		size_t vp = base + valOff;
		a.objId = r.u32(vp);   // D3DX stores string values in the object table; the value is the object id
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
	r.u32(p);              // shared object count
	uint32_t nObjs = r.u32(p);
	(void)nObjs;
	if (!r.ok || nParam > 4096 || nTech > 512) return false;
	for (uint32_t i = 0; i < nParam; i++)
	{
		FxParam prm;
		uint32_t tdOff = r.u32(p);
		prm.dv = r.u32(p);
		prm.flags = r.u32(p);
		uint32_t nAnn = r.u32(p);
		if (!r.ok || nAnn > 256 || !typedefAt(r, base, tdOff, prm.ty)) return false;
		prm.name = prm.ty.name; prm.semantic = prm.ty.semantic; prm.type = prm.ty.type; prm.cls = prm.ty.cls;
		prm.rows = prm.ty.rows; prm.cols = prm.ty.cols; prm.elements = prm.ty.elements; prm.members = (uint32_t)prm.ty.members.size();
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
			for (uint32_t j = 0; j < st; j++)
			{
				FxState s;
				s.op = r.u32(p); s.index = r.u32(p);
				uint32_t td = r.u32(p);
				s.vo = r.u32(p);
				FxType ty;
				if (r.ok && typedefAt(r, base, td, ty)) { s.typeCls = ty.cls; s.typeType = ty.type; }
				ps.sts.push_back(s);
			}
			if (!r.ok) return false;
			t.passes.push_back(std::move(ps));
		}
		fx.techs.push_back(std::move(t));
	}
	fx.base = hdr;
	if (!r.ok) return false;
	// object table and resource records (optional: a truncated tail only disables shaders)
	uint32_t nStr = r.u32(p), nRes = r.u32(p);
	if (r.ok && nStr < 100000 && nRes < 100000)
	{
		bool ok = true;
		for (uint32_t k = 0; k < nStr && ok; k++)
		{
			uint32_t id = r.u32(p), sz = r.u32(p);
			if (!r.ok || p + sz > n) { ok = false; break; }
			fx.objs[id] = {(uint32_t)p, sz};
			p += (sz + 3) & ~3u;
		}
		for (uint32_t k = 0; k < nRes && ok; k++)
		{
			FxRes e;
			e.tech = r.u32(p); e.pass = r.u32(p); e.el = r.u32(p); e.state = r.u32(p); e.usage = r.u32(p);
			e.size = r.u32(p);
			if (!r.ok || p + e.size > n) { ok = false; break; }
			e.off = (uint32_t)p;
			p += (e.size + 3) & ~3u;
			fx.res.push_back(e);
		}
	}
	// resolve string annotations and string parameter defaults through the object table
	auto cstr = [&](uint32_t id) -> std::string {
		auto it = fx.objs.find(id);
		if (it == fx.objs.end()) return "";
		const char* sp = (const char*)d + it->second.first;
		return std::string(sp, strnlen(sp, it->second.second));
	};
	auto fix = [&](std::vector<FxAnn>& v) { for (auto& a : v) if (a.type == 4 && a.objId != 0xffffffffu) a.str = cstr(a.objId); };
	for (auto& pr : fx.params)
	{
		fix(pr.anns);
		if (pr.type == 4 && pr.dv && base + pr.dv + 4 <= n) { uint32_t id; memcpy(&id, d + base + pr.dv, 4); pr.str = cstr(id); }
	}
	for (auto& t : fx.techs) { fix(t.anns); for (auto& ps : t.passes) fix(ps.anns); }
	return true;
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
		if (fxparse::tryParse(d, n, hdr, t) && !t.techs.empty())
		{
			t.blob.assign(d, d + n);
			fx = std::move(t);
			return true;
		}
	}
	return false;
}
}   // namespace d3d
