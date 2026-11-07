// ID3DXEffect: parameter storage and handles, technique/pass application onto the device state.
#include <set>
#include <mutex>
#include <tuple>

#include "d3d9_fxrt.h"
#include "d3d9_int.h"
#include "d3dx_fx.h"
#include "gfx.h"

using namespace rt;

namespace d3d
{
namespace
{
struct HRec { FxInfo* fx; int kind; uint32_t a, b, c; };   // kind: 1 param, 2 tech, 3 pass, 4/5/6 annotation of param/tech/pass
std::vector<HRec> hrecs;
std::map<std::tuple<FxInfo*, int, uint32_t, uint32_t, uint32_t>, uint32_t> hmap;
std::mutex hMu;
constexpr uint32_t HBase = 0x7e000000;

uint32_t mkHandle(FxInfo* fx, int kind, uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
	std::lock_guard<std::mutex> l(hMu);
	auto k = std::make_tuple(fx, kind, a, b, c);
	auto it = hmap.find(k);
	if (it != hmap.end()) return it->second;
	hrecs.push_back({fx, kind, a, b, c});
	uint32_t h = HBase + 4 * (uint32_t)hrecs.size();
	hmap[k] = h;
	return h;
}
const HRec* getRec(uint32_t h)
{
	if (h <= HBase || h > HBase + 0x01000000) return nullptr;
	std::lock_guard<std::mutex> l(hMu);
	uint32_t i = (h - HBase) / 4 - 1;
	return i < hrecs.size() ? &hrecs[i] : nullptr;
}
FxInfo* F(Obj* o) { return o->fx.get(); }
// Resolves a technique handle (record or name string).
const FxTech* techOf(Obj* o, uint32_t h, uint32_t* idx = nullptr)
{
	FxInfo* fx = F(o);
	if (!fx) return nullptr;
	if (const HRec* r = getRec(h)) { if (r->kind == 2 && r->fx == fx && r->a < fx->techs.size()) { if (idx) *idx = r->a; return &fx->techs[r->a]; } return nullptr; }
	if (h) { std::string n = gstr(h); for (size_t i = 0; i < fx->techs.size(); i++) if (fx->techs[i].name == n) { if (idx) *idx = (uint32_t)i; return &fx->techs[i]; } }
	return nullptr;
}
const FxAnn* annOf(uint32_t h)
{
	const HRec* r = getRec(h);
	if (!r || r->kind < 4) return nullptr;
	FxInfo* fx = r->fx;
	if (r->kind == 4) return r->a < fx->params.size() && r->b < fx->params[r->a].anns.size() ? &fx->params[r->a].anns[r->b] : nullptr;
	if (r->kind == 5) return r->a < fx->techs.size() && r->b < fx->techs[r->a].anns.size() ? &fx->techs[r->a].anns[r->b] : nullptr;
	return r->a < fx->techs.size() && r->b < fx->techs[r->a].passes.size() && r->c < fx->techs[r->a].passes[r->b].anns.size() ? &fx->techs[r->a].passes[r->b].anns[r->c] : nullptr;
}
const std::vector<FxAnn>* annListOf(Obj* o, uint32_t h, int* kind, uint32_t* a, uint32_t* b)
{
	FxInfo* fx = F(o);
	if (!fx) return nullptr;
	if (const HRec* r = getRec(h))
	{
		if (r->fx != fx) return nullptr;
		*a = r->a; *b = r->b;
		if (r->kind == 1 && r->a < fx->params.size()) { *kind = 4; return &fx->params[r->a].anns; }
		if (r->kind == 2 && r->a < fx->techs.size()) { *kind = 5; return &fx->techs[r->a].anns; }
		if (r->kind == 3 && r->a < fx->techs.size() && r->b < fx->techs[r->a].passes.size()) { *kind = 6; return &fx->techs[r->a].passes[r->b].anns; }
		return nullptr;
	}
	if (h) { std::string n = gstr(h); for (size_t i = 0; i < fx->params.size(); i++) if (fx->params[i].name == n) { *kind = 4; *a = (uint32_t)i; *b = 0; return &fx->params[i].anns; } }
	return nullptr;
}


// ---- sub-parameter references (members, elements): a handle carries the parameter index, scalar offset and a type id
std::deque<FxType> gSynth;
std::vector<const FxType*> gTypes{nullptr};
std::map<const FxType*, uint32_t> gTypeIds;
uint32_t typeId(const FxType* t)
{
	auto it = gTypeIds.find(t);
	if (it != gTypeIds.end()) return it->second;
	gTypes.push_back(t);
	return gTypeIds[t] = (uint32_t)gTypes.size() - 1;
}
const FxType* oneElement(const FxType& t)
{
	std::lock_guard<std::mutex> l(hMu);
	gSynth.push_back(t);
	gSynth.back().elements = 0;
	return &gSynth.back();
}
struct PRef
{
	FxParam* p = nullptr;
	uint32_t off = 0;           // scalar offset into p->val
	const FxType* ty = nullptr;
	bool valid() const { return p != nullptr; }
};
PRef refOf(Obj* o, uint32_t h)
{
	PRef r;
	FxInfo* fx = F(o);
	if (!fx) return r;
	if (const HRec* rc = getRec(h))
	{
		if (rc->kind != 1 || rc->fx != fx || rc->a >= fx->params.size()) return r;
		r.p = &fx->params[rc->a];
		r.off = rc->b;
		{
			std::lock_guard<std::mutex> l(hMu);
			r.ty = rc->c && rc->c < gTypes.size() ? gTypes[rc->c] : &r.p->ty;
		}
		return r;
	}
	if (h)   // plain name
	{
		std::string n = gstr(h);
		int i = fxrt::findParam(*fx, n);
		if (i >= 0) { r.p = &fx->params[i]; r.ty = &r.p->ty; }
	}
	return r;
}

// ---- shared parameters (D3DXEffectPool): a write to a shared parameter of one effect is copied to the same-named
// parameter of every other effect before the next draw
static std::mutex gShareMu;
static std::vector<std::weak_ptr<FxInfo>> gAllFx;
static std::vector<std::pair<FxInfo*, FxParam*>> gDirtyShared;
}   // anonymous namespace (reopened below)
void fxRegister(const std::shared_ptr<FxInfo>& fx)
{
	std::lock_guard<std::mutex> l(gShareMu);
	gAllFx.push_back(fx);
	std::string sh;
	for (auto& p : fx->params) if (p.flags & 1) sh += (sh.empty() ? "" : ",") + p.name;
	log("   shared params: %s", sh.c_str());
}
namespace
{
PRef refOfSet(Obj* o, uint32_t h)
{
	PRef r = refOf(o, h);
	if (r.valid() && (r.p->flags & 1))
	{
		std::lock_guard<std::mutex> l(gShareMu);
		FxInfo* fx = F(o);
		for (auto& d : gDirtyShared) if (d.second == r.p) return r;
		gDirtyShared.push_back({fx, r.p});
	}
	return r;
}
static void flushShared()
{
	std::lock_guard<std::mutex> l(gShareMu);
	if (gDirtyShared.empty()) return;
	for (auto& d : gDirtyShared)
		for (auto& w : gAllFx)
		{
			auto fx2 = w.lock();
			if (!fx2 || fx2.get() == d.first) continue;
			int i = fxrt::findParam(*fx2, d.second->name);
			if (i < 0) continue;
			FxParam& q = fx2->params[i];
			if (d.second->name == "ViewProjection")
			{
				static int fl = 0;
				if (fl++ < 12) log("fx: flush ViewProjection %p -> %p sizes %zu/%zu v0 %g", (void*)d.first, (void*)fx2.get(), d.second->val.size(), q.val.size(), d.second->val.empty() ? 0.0 : (double)*(const float*)&d.second->val[0]);
			}
			if (q.val.size() == d.second->val.size()) q.val = d.second->val;
			q.texObj = d.second->texObj;
		}
	gDirtyShared.clear();
}
uint32_t handleOf(Obj* o, const PRef& r)
{
	FxInfo* fx = F(o);
	uint32_t a = (uint32_t)(r.p - fx->params.data());
	uint32_t c = r.ty == &r.p->ty ? 0 : typeId(r.ty);
	return mkHandle(fx, 1, a, r.off, c);
}
// resolve "Name[2].Member[1]" relative to `base` (invalid base: global scope)
bool resolvePath(FxInfo* fx, PRef base, const std::string& path, PRef& out)
{
	size_t i = 0;
	PRef cur = base;
	bool first = !base.valid();
	while (i < path.size())
	{
		size_t e = i;
		while (e < path.size() && path[e] != '.' && path[e] != '[') e++;
		std::string id = path.substr(i, e - i);
		if (!id.empty())
		{
			if (first)
			{
				int k = fxrt::findParam(*fx, id);
				if (k < 0) return false;
				cur.p = &fx->params[k];
				cur.ty = &cur.p->ty;
				cur.off = 0;
			}
			else
			{
				if (cur.ty->cls != 5 || cur.ty->elements) return false;
				uint32_t off = 0;
				bool found = false;
				for (auto& m : cur.ty->members)
				{
					if (m.name == id) { cur.off += off; cur.ty = &m; found = true; break; }
					off += m.scalars() * std::max(1u, m.elements);
				}
				if (!found) return false;
			}
			first = false;
		}
		i = e;
		if (i < path.size() && path[i] == '[')
		{
			size_t close = path.find(']', i);
			if (close == std::string::npos || first) return false;
			uint32_t idx = (uint32_t)atoi(path.c_str() + i + 1);
			if (!cur.ty->elements || idx >= cur.ty->elements) return false;
			cur.off += idx * cur.ty->scalars();
			cur.ty = oneElement(*cur.ty);
			i = close + 1;
		}
		if (i < path.size() && path[i] == '.') i++;
	}
	if (!cur.valid()) return false;
	out = cur;
	return true;
}

// ---- value access
static uint32_t leafType(const FxType* t) { return t->cls == 5 ? 0 : t->type; }
static uint32_t toLeaf(uint32_t leaf, uint32_t srcType, uint32_t v)   // convert a scalar to the parameter's scalar type
{
	if (leaf < 1 || leaf > 3 || leaf == srcType) return v;
	float f;
	int32_t i;
	if (srcType == 3) { memcpy(&f, &v, 4); i = (int32_t)f; }
	else { i = (int32_t)v; f = srcType == 1 ? (v ? 1.f : 0.f) : (float)i; }
	if (leaf == 3) { memcpy(&v, &f, 4); return v; }
	if (leaf == 2) return (uint32_t)i;
	return srcType == 3 ? (f != 0.f) : (i != 0);
}
static void writeVals(const PRef& r, const uint32_t* src, uint32_t n, uint32_t srcType)
{
	uint32_t leaf = leafType(r.ty);
	for (uint32_t i = 0; i < n && r.off + i < r.p->val.size(); i++) r.p->val[r.off + i] = toLeaf(leaf, srcType, src[i]);
}
static void readVals(const PRef& r, uint32_t* dst, uint32_t n, uint32_t dstType)
{
	uint32_t leaf = leafType(r.ty);
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t v = r.off + i < r.p->val.size() ? r.p->val[r.off + i] : 0;
		dst[i] = (leaf >= 1 && leaf <= 3) ? toLeaf(dstType, leaf, v) : v;
	}
}
// matrices: m is a row-major 4x4 in host memory
static void putMatrix(const PRef& r, const float* m, bool transpose)
{
	const FxType* t = r.ty;
	if (t->cls != 2 && t->cls != 3)
	{
		uint32_t n = t->scalars();
		std::vector<uint32_t> tmp(n);
		memcpy(tmp.data(), m, 4 * std::min(n, 16u));
		writeVals(r, tmp.data(), n, 3);
		return;
	}
	uint32_t R = t->rows, C = t->cols, leaf = leafType(t);
	for (uint32_t row = 0; row < R; row++)
		for (uint32_t col = 0; col < C; col++)
		{
			float v = transpose ? m[col * 4 + row] : m[row * 4 + col];
			uint32_t bits;
			memcpy(&bits, &v, 4);
			uint32_t idx = r.off + (t->cls == 3 ? row * C + col : col * R + row);
			if (idx < r.p->val.size()) r.p->val[idx] = toLeaf(leaf, 3, bits);
		}
}
static void getMatrix(const PRef& r, float* m, bool transpose)
{
	for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f;
	const FxType* t = r.ty;
	if (t->cls != 2 && t->cls != 3) return;
	uint32_t R = t->rows, C = t->cols, leaf = leafType(t);
	for (uint32_t row = 0; row < R; row++)
		for (uint32_t col = 0; col < C; col++)
		{
			uint32_t idx = r.off + (t->cls == 3 ? row * C + col : col * R + row);
			uint32_t v = idx < r.p->val.size() ? r.p->val[idx] : 0;
			v = toLeaf(3, leaf, v);
			float f;
			memcpy(&f, &v, 4);
			if (transpose) m[col * 4 + row] = f; else m[row * 4 + col] = f;
		}
}

H("ID3DXEffect::GetParameter")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(newStr("param")); return; }
	uint32_t idx = ARG(2);
	if (!ARG(1)) { SET(idx < fx->params.size() ? mkHandle(fx, 1, idx) : 0); return; }
	PRef b = refOf(o, ARG(1));
	if (!b.valid()) { SET(0); return; }
	PRef r = b;
	if (b.ty->elements)   // element of an array
	{
		if (idx >= b.ty->elements) { SET(0); return; }
		r.off += idx * b.ty->scalars();
		r.ty = oneElement(*b.ty);
	}
	else if (b.ty->cls == 5)   // member of a struct
	{
		if (idx >= b.ty->members.size()) { SET(0); return; }
		uint32_t off = 0;
		for (uint32_t i = 0; i < idx; i++) off += b.ty->members[i].scalars() * std::max(1u, b.ty->members[i].elements);
		r.off += off;
		r.ty = &b.ty->members[idx];
	}
	else { SET(0); return; }
	SET(handleOf(o, r));
} END;
H("ID3DXEffect::GetParameterByName")
{
	FxInfo* fx = F(o);
	if (!ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	if (!fx) { SET(newStr(n)); return; }
	PRef base;
	if (ARG(1)) { base = refOf(o, ARG(1)); if (!base.valid()) { SET(0); return; } }
	PRef r;
	if (resolvePath(fx, base, n, r)) SET(handleOf(o, r)); else SET(0);
} END;
H("ID3DXEffect::GetParameterBySemantic")
{
	FxInfo* fx = F(o);
	if (!ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	if (!fx) { SET(newStr(n)); return; }
	for (size_t i = 0; i < fx->params.size(); i++) if (strcasecmp(fx->params[i].semantic.c_str(), n.c_str()) == 0) { SET(mkHandle(fx, 1, (uint32_t)i)); return; }
	SET(0);
} END;
H("ID3DXEffect::GetParameterElement")
{
	PRef b = refOf(o, ARG(1));
	if (!b.valid()) { SET(F(o) ? 0 : newStr("elem")); return; }
	uint32_t idx = ARG(2);
	if (!b.ty->elements || idx >= b.ty->elements) { SET(0); return; }
	PRef r = b;
	r.off += idx * b.ty->scalars();
	r.ty = oneElement(*b.ty);
	SET(handleOf(o, r));
} END;
H("ID3DXEffect::GetParameterDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 44);
	PRef pr = refOf(o, ARG(1));
	const FxAnn* an = pr.valid() ? nullptr : annOf(ARG(1));
	if (!pr.valid() && !an) { SET(0x8876086c); return; }   // D3DERR_INVALIDCALL
	std::string name, sem;
	uint32_t cls, type, rows, cols, el, members = 0, flags = 0, nanns = 0, bytes;
	if (pr.valid())
	{
		const FxType& t = *pr.ty;
		name = t.name; sem = t.semantic;
		if (pr.ty == &pr.p->ty) { name = pr.p->name; sem = pr.p->semantic; flags = pr.p->flags; nanns = (uint32_t)pr.p->anns.size(); }
		cls = t.cls; type = t.type; rows = t.rows; cols = t.cols; el = t.elements; members = (uint32_t)t.members.size();
		bytes = 4 * t.scalars() * std::max(1u, t.elements);
	}
	else
	{
		name = an->name; cls = an->cls; type = an->type; rows = an->rows; cols = an->cols; el = an->elements;
		bytes = 4 * rows * cols * std::max(1u, el);
	}
	guest::store<uint32_t>(p, newStr(name));
	guest::store<uint32_t>(p + 4, sem.empty() ? 0 : newStr(sem));
	guest::store<uint32_t>(p + 8, cls);
	guest::store<uint32_t>(p + 12, type);
	guest::store<uint32_t>(p + 16, rows);
	guest::store<uint32_t>(p + 20, cols);
	guest::store<uint32_t>(p + 24, el);
	guest::store<uint32_t>(p + 28, nanns);
	guest::store<uint32_t>(p + 32, members);
	guest::store<uint32_t>(p + 36, flags);
	guest::store<uint32_t>(p + 40, bytes);
	SET(0);
} END;

// ---- setters
H("ID3DXEffect::SetValue")
{
	PRef r = refOfSet(o, ARG(1));
	if (r.valid() && ARG(2))
	{
		if (r.ty->cls == 4)
		{
			if (r.p->type >= 5 && r.p->type <= 9)
			{
				r.p->texObj = guest::load<uint32_t>(ARG(2));
				static int nl = 0;
				if (nl++ < 40) log("fx: SetValue texture '%s' -> %x", r.p->name.c_str(), r.p->texObj);
			}
			SET(0);
			return;
		}
		uint32_t n = std::min(ARG(3) / 4, (uint32_t)r.p->val.size() - std::min<uint32_t>(r.off, (uint32_t)r.p->val.size()));
		memcpy(r.p->val.data() + r.off, guest::host(ARG(2)), 4ull * n);
	}
	SET(0);
} END;
H("ID3DXEffect::SetBool") { PRef r = refOfSet(o, ARG(1)); if (r.valid()) { uint32_t v = ARG(2) ? 1 : 0; writeVals(r, &v, 1, 1); } SET(0); } END;
H("ID3DXEffect::SetInt") { PRef r = refOfSet(o, ARG(1)); if (r.valid()) { uint32_t v = ARG(2); writeVals(r, &v, 1, 2); } SET(0); } END;
H("ID3DXEffect::SetFloat") { PRef r = refOfSet(o, ARG(1)); if (r.valid()) { uint32_t v = ARG(2); writeVals(r, &v, 1, 3); } SET(0); } END;
H("ID3DXEffect::SetBoolArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) { std::vector<uint32_t> v(ARG(3)); for (uint32_t i = 0; i < ARG(3); i++) v[i] = guest::load<uint32_t>(ARG(2) + 4 * i) ? 1 : 0; writeVals(r, v.data(), ARG(3), 1); } SET(0); } END;
H("ID3DXEffect::SetIntArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) writeVals(r, (const uint32_t*)guest::host(ARG(2)), ARG(3), 2); SET(0); } END;
H("ID3DXEffect::SetFloatArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) writeVals(r, (const uint32_t*)guest::host(ARG(2)), ARG(3), 3); SET(0); } END;
static void noteBad(const char* m, uint32_t h, const PRef& r)
{
	static int n = 0;
	if (!r.valid() && n >= 25) return;
	if (!r.valid()) { n++; const HRec* rc = getRec(h); log("fx: %s with invalid handle %x (rec %p kind %d)", m, h, (const void*)rc, rc ? (int)rc->kind : -1); }
	else
	{
		static std::set<const void*> seen;
		static int m2 = 0;
		if (m2 < 120 && seen.insert(r.p).second) { m2++; log("fx: %s writes param '%s' type %u cls %u rows %u cols %u smpTex %u", m, r.p->name.c_str(), r.p->type, r.p->cls, r.p->rows, r.p->cols, r.p->smpTex); }
	}
	if (!r.valid()) {}
	else if ((r.p->cls == 4 || r.p->type >= 5) && n < 25) { n++; log("fx: %s on object param '%s' type %u cls %u", m, r.p->name.c_str(), r.p->type, r.p->cls); }
}
H("ID3DXEffect::SetVector")
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetVector", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		writeVals(r, (const uint32_t*)guest::host(ARG(2)), std::min(r.ty->scalars(), 4u), 3);
		if (r.p->name == "EyePosition")
		{
			static int n = 0; const float* v = (const float*)guest::host(ARG(2));
			if ((v[0] != 0.f || v[1] != 0.f || v[2] != 0.f) && n++ < 6) log("fx: EyePosition %g %g %g", v[0], v[1], v[2]);
		}
	}
	SET(0);
} END;
H("ID3DXEffect::SetVectorArray")
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetVectorArray", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		uint32_t per = std::min(r.ty->scalars(), 4u), stride = r.ty->scalars();
		for (uint32_t i = 0; i < ARG(3); i++)
		{
			PRef e = r;
			e.off += i * stride;
			writeVals(e, (const uint32_t*)guest::host(ARG(2) + 16 * i), per, 3);
		}
	}
	SET(0);
} END;
static void setMatrices(CPU* c, Obj* o, bool transpose, bool array, bool pointers)
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetMatrix*", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		uint32_t n = array ? ARG(3) : 1, stride = r.ty->scalars();
		for (uint32_t i = 0; i < n; i++)
		{
			PRef e = r;
			e.off += i * stride;
			const float* m = pointers ? (const float*)guest::host(guest::load<uint32_t>(ARG(2) + 4 * i)) : (const float*)guest::host(ARG(2) + 64 * i);
			putMatrix(e, m, transpose);
			if (r.p->name == "ViewProjection")
			{
				static int total = 0, nonzero = 0, shown = 0;
				bool nz = false;
				for (int k = 0; k < 16; k++) if (m[k] != 0.f) nz = true;
				total++;
				if (nz) nonzero++;
				{
					static uint32_t W = 0; static bool inited = false;
					if (!inited && total >= 3000) { inited = true; W = ARG(2); log("fx: watching the ViewProjection buffer at %08x", W); }
					if (W && ARG(2) == W) rt::watchArm(W);
				}
				if (total == 1 || total == 3000 || total == 20000 || total == 40000)
				{
					std::string st;
					for (int k = 0; k < 24; k++) { char b[16]; snprintf(b, sizeof b, " %08x", guest::load<uint32_t>(c->r[ESP] + 4 * k)); st += b; }
					log("fx: ViewProjection #%d caller ret=%08x mptr=%08x stack:%s", total, guest::load<uint32_t>(c->r[ESP]), ARG(2), st.c_str());
				}
				if ((nz && shown++ < 8) || total % 2000 == 0)
					log("fx: ViewProjection set #%d (nonzero %d) on fx %p transpose %d [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", total, nonzero, (void*)F(o), (int)transpose, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
			}
		}
	}
	SET(0);
}
H("ID3DXEffect::SetMatrix") { setMatrices(c, o, false, false, false); } END;
H("ID3DXEffect::SetMatrixArray") { setMatrices(c, o, false, true, false); } END;
H("ID3DXEffect::SetMatrixPointerArray") { setMatrices(c, o, false, true, true); } END;
H("ID3DXEffect::SetMatrixTranspose") { setMatrices(c, o, true, false, false); } END;
H("ID3DXEffect::SetMatrixTransposeArray") { setMatrices(c, o, true, true, false); } END;
H("ID3DXEffect::SetMatrixTransposePointerArray") { setMatrices(c, o, true, true, true); } END;
H("ID3DXEffect::SetTexture")
{
	PRef r = refOfSet(o, ARG(1));
	if (r.valid()) r.p->texObj = ARG(2);
	{
		static int nl = 0;
		if (nl++ < 40) log("fx: SetTexture handle %x valid %d name '%s' tex %x", ARG(1), (int)r.valid(), r.valid() ? r.p->name.c_str() : "", ARG(2));
	}
	SET(0);
} END;
H("ID3DXEffect::SetString") { SET(0); } END;
H("ID3DXEffect::SetArrayRange") { SET(0); } END;

// ---- getters
H("ID3DXEffect::GetValue")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid() && ARG(2)) { uint32_t n = std::min(ARG(3) / 4, (uint32_t)r.p->val.size() - std::min<uint32_t>(r.off, (uint32_t)r.p->val.size())); memcpy(guest::host(ARG(2)), r.p->val.data() + r.off, 4ull * n); }
	SET(0);
} END;
H("ID3DXEffect::GetBool")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 1); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), an && !an->vals.empty() && an->vals[0] ? 1 : 0);
	SET(0);
} END;
H("ID3DXEffect::GetInt")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 2); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), an && !an->vals.empty() ? (an->type == 3 ? (uint32_t)(int)*(float*)&an->vals[0] : an->vals[0]) : 0);
	SET(0);
} END;
H("ID3DXEffect::GetFloat")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 3); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	float v = 0.f;
	if (an && !an->vals.empty()) { if (an->type == 3) memcpy(&v, &an->vals[0], 4); else v = (float)(int)an->vals[0]; }
	if (ARG(2)) guest::store<float>(ARG(2), v);
	SET(0);
} END;
H("ID3DXEffect::GetFloatArray") { PRef r = refOf(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 3); SET(0); } END;
H("ID3DXEffect::GetIntArray") { PRef r = refOf(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 2); SET(0); } END;
H("ID3DXEffect::GetBoolArray") { PRef r = refOf(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 1); SET(0); } END;
H("ID3DXEffect::GetVector")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid() && ARG(2)) { float v[4] = {0, 0, 0, 0}; readVals(r, (uint32_t*)v, std::min(r.ty->scalars(), 4u), 3); memcpy(guest::host(ARG(2)), v, 16); }
	SET(0);
} END;
H("ID3DXEffect::GetMatrix")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid() && ARG(2)) { float m[16]; getMatrix(r, m, false); memcpy(guest::host(ARG(2)), m, 64); }
	SET(0);
} END;
H("ID3DXEffect::GetMatrixTranspose")
{
	PRef r = refOf(o, ARG(1));
	if (r.valid() && ARG(2)) { float m[16]; getMatrix(r, m, true); memcpy(guest::host(ARG(2)), m, 64); }
	SET(0);
} END;
H("ID3DXEffect::GetTexture")
{
	PRef r = refOf(o, ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), r.valid() ? r.p->texObj : 0);
	if (r.valid() && r.p->texObj) if (Obj* t = objectAt(r.p->texObj)) t->ref++;
	SET(0);
} END;

// ---- technique / pass application
struct PassCtx { FxInfo* fx = nullptr; uint32_t tech = 0, pass = 0; bool active = false; };
static PassCtx gPass;

static std::string cstrAt(const FxInfo& fx, const FxRes& r)
{
	const char* s = (const char*)fx.blob.data() + r.off;
	return std::string(s, strnlen(s, r.size));
}

static void packConst(const FxInfo& fx, const CtabConst& c, ShaderObj* sh, bool isVs)
{
	int k = fxrt::findParam(fx, c.name);
	if (k < 0) return;
	const FxParam& p = fx.params[k];
	if (p.ty.cls == 4 || p.val.empty() || p.skipConst) return;
	float* fregs = isVs ? dev.vsF : dev.psF;
	int32_t* iregs = isVs ? dev.vsI : dev.psI;
	int32_t* bregs = isVs ? dev.vsB : dev.psB;
	uint32_t maxF = isVs ? 256 : 224;
	if (c.regSet == 2)
	{
		if (c.regIndex >= maxF) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 2, c.regIndex, (uint32_t*)fregs, maxF);
	}
	else if (c.regSet == 1)
	{
		if (c.regIndex >= 16) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 1, c.regIndex, (uint32_t*)iregs, 16);
	}
	else if (c.regSet == 0)
	{
		if (c.regIndex >= 16) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 0, c.regIndex, (uint32_t*)bregs, 16);
	}
	(void)sh;
}

static void applyShaderState(FxInfo& fx, const FxPass& ps, uint32_t tech, uint32_t pass, uint32_t si)
{
	const FxState& st = ps.sts[si];
	fxrt::StateInfo info = fxrt::stateInfo(st.op);
	if (info.cls == fxrt::SC_VS || info.cls == fxrt::SC_PS)
	{
		fxrt::ShaderRef ref;
		ShaderObj* sh = nullptr;
		if (fxrt::resolveShader(fx, tech, pass, si, ref)) sh = getShader(ref.code, ref.size);
		if (info.cls == fxrt::SC_VS) dev.vs = sh; else dev.ps = sh;
		return;
	}
	// value: constant at vo, or a parameter reference
	uint32_t value = fx.u32(fx.base + st.vo);
	for (auto& r : fx.res)
		if (r.tech == tech && r.pass == pass && r.state == si)
		{
			if (r.usage == 1)
			{
				std::string path = cstrAt(fx, r);
				PRef ref;
				if (resolvePath(&fx, PRef(), path, ref) && ref.off < ref.p->val.size())
					value = toLeaf(st.typeType, ref.ty->type, ref.p->val[ref.off]);
			}
			break;
		}
	switch (info.cls)
	{
	case fxrt::SC_RS:
		if (info.id < 256) dev.rs[info.id] = value;
		break;
	default: break;
	}
}

void fxApplyPass(Obj* o)
{
	flushShared();
	FxInfo* fx = F(o);
	if (!fx || !gPass.active || gPass.fx != fx) return;
	const FxPass& ps = fx->techs[gPass.tech].passes[gPass.pass];
	for (uint32_t i = 0; i < ps.sts.size(); i++) applyShaderState(*fx, ps, gPass.tech, gPass.pass, i);
	// samplers and constants required by the selected shaders
	for (int stage = 0; stage < 2; stage++)
	{
		ShaderObj* sh = stage == 0 ? dev.vs : dev.ps;
		if (!sh || !sh->ok) continue;
		for (auto& c : sh->info.ctab)
		{
			if (c.regSet == 3)
			{
				int k = fxrt::findParam(*fx, c.name);
				if (k < 0 || c.regIndex >= 16) continue;
				const FxParam& sp = fx->params[k];
				{
					static int nl = 0;
					if (nl++ < 40) log("fx: sampler '%s' reg %u param %d smpTex %u texObj %x linkedTex %x", c.name.c_str(), c.regIndex, k, sp.smpTex, sp.texObj, sp.smpTex ? fx->params[sp.smpTex - 1].texObj : 0u);
				}
				if (sp.smpTex)
				{
					uint32_t t = fx->params[sp.smpTex - 1].texObj;
					if (t) dev.tex[c.regIndex] = objectAt(t);
				}
				else if (sp.texObj) dev.tex[c.regIndex] = objectAt(sp.texObj);
				for (auto& s : sp.smp) if (s.op < 16) dev.samp[c.regIndex][s.op] = s.v;
			}
			else packConst(*fx, c, sh, stage == 0);
		}
	}
}
void fxCommit(Obj* o) { fxApplyPass(o); }

H("ID3DXEffect::Begin")
{
	FxInfo* fx = F(o);
	if (ARG(1)) guest::store<uint32_t>(ARG(1), fx && fx->current < fx->techs.size() ? (uint32_t)fx->techs[fx->current].passes.size() : 1);
	if (fx) { gPass.fx = fx; gPass.tech = fx->current; gPass.pass = 0; gPass.active = false; }
	SET(0);
} END;
H("ID3DXEffect::BeginPass")
{
	FxInfo* fx = F(o);
	if (fx && fx->current < fx->techs.size() && ARG(1) < fx->techs[fx->current].passes.size())
	{
		gPass.fx = fx; gPass.tech = fx->current; gPass.pass = ARG(1); gPass.active = true;
		fxApplyPass(o);
	}
	SET(0);
} END;
H("ID3DXEffect::CommitChanges") { fxApplyPass(o); SET(0); } END;
H("ID3DXEffect::EndPass") { SET(0); } END;
H("ID3DXEffect::End") { gPass.active = false; SET(0); } END;
H("ID3DXEffect::OnLostDevice") { SET(0); } END;
H("ID3DXEffect::OnResetDevice") { SET(0); } END;
H("ID3DXEffect::GetDesc")
{
	uint32_t p = ARG(1);
	memset(guest::host(p), 0, 16);
	FxInfo* fx = F(o);
	guest::store<uint32_t>(p + 4, fx ? (uint32_t)fx->params.size() : 0);
	guest::store<uint32_t>(p + 8, fx ? (uint32_t)fx->techs.size() : 1);
	SET(0);
} END;
H("ID3DXEffect::GetTechniqueDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 12);
	if (const FxTech* t = techOf(o, ARG(1)))
	{
		guest::store<uint32_t>(p, newStr(t->name));
		guest::store<uint32_t>(p + 4, (uint32_t)t->passes.size());
		guest::store<uint32_t>(p + 8, (uint32_t)t->anns.size());
	}
	else { guest::store<uint32_t>(p, newStr("T")); guest::store<uint32_t>(p + 4, 1); }
	SET(0);
} END;
H("ID3DXEffect::GetPassDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 16);
	if (const HRec* r = getRec(ARG(1)); r && r->kind == 3 && r->fx == F(o) && r->a < r->fx->techs.size() && r->b < r->fx->techs[r->a].passes.size())
	{
		const FxPass& ps = r->fx->techs[r->a].passes[r->b];
		guest::store<uint32_t>(p, newStr(ps.name));
		guest::store<uint32_t>(p + 4, (uint32_t)ps.anns.size());
	}
	SET(0);
} END;
H("ID3DXEffect::GetTechnique")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(0x7e01); return; }
	SET(ARG(1) < fx->techs.size() ? mkHandle(fx, 2, ARG(1)) : 0);
} END;
H("ID3DXEffect::GetTechniqueByName")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(0x7e01); return; }
	uint32_t idx;
	SET(ARG(1) && techOf(o, ARG(1), &idx) ? mkHandle(fx, 2, idx) : 0);
} END;
H("ID3DXEffect::GetPass")
{
	FxInfo* fx = F(o);
	uint32_t ti;
	if (!fx) { SET(0x7e02); return; }
	const FxTech* t = techOf(o, ARG(1), &ti);
	SET(t && ARG(2) < t->passes.size() ? mkHandle(fx, 3, ti, ARG(2)) : 0);
} END;
H("ID3DXEffect::GetPassByName")
{
	FxInfo* fx = F(o);
	uint32_t ti;
	if (!fx) { SET(0x7e02); return; }
	const FxTech* t = techOf(o, ARG(1), &ti);
	if (t && ARG(2))
	{
		std::string n = gstr(ARG(2));
		for (size_t i = 0; i < t->passes.size(); i++) if (t->passes[i].name == n) { SET(mkHandle(fx, 3, ti, (uint32_t)i)); return; }
	}
	SET(0);
} END;
H("ID3DXEffect::GetAnnotation")
{
	int kind; uint32_t a, b;
	const std::vector<FxAnn>* l = annListOf(o, ARG(1), &kind, &a, &b);
	if (!l || ARG(2) >= l->size()) { SET(0); return; }
	SET(kind == 6 ? mkHandle(F(o), kind, a, b, ARG(2)) : mkHandle(F(o), kind, a, ARG(2)));
} END;
H("ID3DXEffect::GetAnnotationByName")
{
	int kind; uint32_t a, b;
	const std::vector<FxAnn>* l = annListOf(o, ARG(1), &kind, &a, &b);
	if (!l || !ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	{ static int cn = 0; if (cn++ < 60) { std::string all; for (auto& a : *l) all += a.name + ","; log("fx: GetAnnotationByName '%s' among [%s]", n.c_str(), all.c_str()); } }
	for (size_t i = 0; i < l->size(); i++)
		if ((*l)[i].name == n)
		{
			SET(kind == 6 ? mkHandle(F(o), kind, a, b, (uint32_t)i) : mkHandle(F(o), kind, a, (uint32_t)i));
			return;
		}
	SET(0);
} END;
H("ID3DXEffect::GetString")
{
	const FxAnn* an = annOf(ARG(1));
	std::string sv = an ? an->str : "";
	if (!an) { PRef pr = refOf(o, ARG(1)); if (pr.valid()) sv = pr.p->str; }
	{ static int n = 0; if (n++ < 80) log("fx: GetString h %x -> %s '%s'", ARG(1), an ? "ann" : "param", sv.c_str()); }
	if (ARG(2)) guest::store<uint32_t>(ARG(2), newStr(sv));
	SET(0);
} END;
H("ID3DXEffect::SetTechnique") { uint32_t idx; if (F(o) && techOf(o, ARG(1), &idx)) F(o)->current = idx; SET(0); } END;
H("ID3DXEffect::GetCurrentTechnique") { FxInfo* fx = F(o); SET(fx && fx->current < fx->techs.size() ? mkHandle(fx, 2, fx->current) : 0x7e01); } END;
H("ID3DXEffect::ValidateTechnique") { SET(0); } END;
H("ID3DXEffect::FindNextValidTechnique")
{
	FxInfo* fx = F(o);
	uint32_t idx = 0, next = 0;
	if (fx && ARG(1) && techOf(o, ARG(1), &idx)) next = idx + 1;
	if (ARG(2)) guest::store<uint32_t>(ARG(2), fx ? (next < fx->techs.size() ? mkHandle(fx, 2, next) : 0) : 0x7e01);
	SET(0);
} END;
H("ID3DXEffect::IsParameterUsed") { SET(1); } END;

}   // anonymous
}   // namespace d3d
