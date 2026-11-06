// Direct3D 9 shader byte code (vs/ps 2.0 and 3.0) -> GLSL ES 3.00 translator.
// Pure C++ (no GL, no guest memory), so it can be unit-tested on the host with every shader of the game.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace d3d
{
struct CtabConst
{
	std::string name;
	uint32_t regSet = 2;       // 0 bool, 1 int4, 2 float4, 3 sampler
	uint32_t regIndex = 0, regCount = 0;
	uint32_t cls = 0, type = 0, rows = 0, cols = 0, elements = 0, members = 0;
};

struct ShaderInfo
{
	bool isVs = false, ok = false;
	int major = 2, minor = 0;
	std::string err;
	std::vector<CtabConst> ctab;
	int samplerType[16];                 // 0 unused, 2 = 2D, 3 = cube, 4 = volume
	std::map<std::string, int> inputs;   // VS: attribute name -> location. PS: varying names consumed
	std::set<std::string> outputs;       // VS: varying names written
	std::string body;                    // GLSL main() contents + declarations that do not depend on linking
	std::string decls;                   // attribute/varying/uniform declarations (sans PS inputs)
	std::vector<std::string> psIns;      // PS: varying names consumed
	bool usesFragCoord = false, usesFace = false, writesDepth = false;
	int numC = 0;
	int psOutputs = 0;                   // PS: bitmask of oC registers written
	ShaderInfo() { memset(samplerType, 0, sizeof samplerType); }
};

namespace shdr
{
static const char* const kUsage[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};

// attribute location for a vertex semantic (shared with the vertex declaration binding)
inline int attrLocation(int usage, int index)
{
	switch (usage)
	{
	case 0: return index == 0 ? 0 : -1;
	case 1: return index == 0 ? 1 : -1;
	case 2: return index == 0 ? 2 : -1;
	case 3: return index == 0 ? 3 : -1;
	case 5: return index < 8 ? 4 + index : -1;
	case 6: return index == 0 ? 12 : -1;
	case 7: return index == 0 ? 13 : -1;
	case 10: return index < 2 ? 14 + index : -1;
	default: return -1;
	}
}

struct Reg
{
	int type = 0, num = 0;
	bool rel = false;
	int relComp = 0;
	uint32_t tok = 0;
};
inline int regType(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

inline std::string fmtFloat(float f)
{
	char b[64];
	if (f != f) return "0.0";
	snprintf(b, sizeof b, "%.9g", f);
	std::string s = b;
	if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
	else if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
	else if (s.find('.') == std::string::npos && s.find('e') != std::string::npos) { size_t e = s.find('e'); s.insert(e, ".0"); }
	return s;
}

inline void parseCtab(const uint8_t* d, size_t n, std::vector<CtabConst>& out)
{
	// d points after the 'CTAB' tag
	if (n < 28) return;
	auto u32 = [&](size_t p) { uint32_t v = 0; if (p + 4 <= n) memcpy(&v, d + p, 4); return v; };
	auto u16 = [&](size_t p) { uint16_t v = 0; if (p + 2 <= n) memcpy(&v, d + p, 2); return (uint32_t)v; };
	uint32_t nc = u32(12), ci = u32(16);
	if (nc > 512) return;
	for (uint32_t i = 0; i < nc; i++)
	{
		size_t p = ci + 20ull * i;
		CtabConst c;
		uint32_t no = u32(p);
		if (no >= n) continue;
		c.name = (const char*)d + no;
		c.regSet = u16(p + 4);
		c.regIndex = u16(p + 6);
		c.regCount = u16(p + 8);
		uint32_t ti = u32(p + 12);
		if (ti + 16 <= n)
		{
			c.cls = u16(ti); c.type = u16(ti + 2); c.rows = u16(ti + 4); c.cols = u16(ti + 6); c.elements = u16(ti + 8); c.members = u16(ti + 10);
		}
		out.push_back(c);
	}
}

struct Translator
{
	ShaderInfo& si;
	const uint32_t* w;
	size_t nw;
	std::string out;
	int indent = 1;
	std::map<int, std::string> defC;       // c# -> "vec4(...)"
	std::map<int, std::string> defI;
	std::map<int, bool> defB;
	std::set<int> temps;
	std::set<int> inputsUsed;
	std::map<int, std::pair<int, int>> inDcl;    // v# -> (usage,index)
	std::map<int, std::pair<int, int>> outDcl;   // o# (vs3) -> (usage,index)
	std::map<int, std::pair<int, int>> psTexDcl; // t# (ps2) / v# (ps3)
	bool usesA0 = false;
	std::set<int> outsWritten;          // vs2 outputs used: key = kind*16+idx
	std::set<int> ocUsed;
	int repN = 0;
	bool vs3 = false;

	Translator(ShaderInfo& s, const uint32_t* words, size_t n) : si(s), w(words), nw(n) {}

	void line(const std::string& s) { out += std::string(indent * 2, ' ') + s + "\n"; }
	bool fail(const std::string& e) { si.err = e; return false; }

	static std::string compName(int c) { return std::string(1, "xyzw"[c & 3]); }

	std::string regName(const Reg& r, bool dst)
	{
		char b[64];
		switch (r.type)
		{
		case 0: temps.insert(r.num); snprintf(b, sizeof b, "r%d", r.num); return b;
		case 1:   // input
			if (si.isVs || si.major >= 3) { inputsUsed.insert(r.num); snprintf(b, sizeof b, "i%d", r.num); return b; }
			inputsUsed.insert(r.num);   // ps_2: v# = color
			snprintf(b, sizeof b, "i%d", r.num);
			return b;
		case 2:   // float const
		{
			if (!r.rel && defC.count(r.num)) { snprintf(b, sizeof b, "d%d", r.num); return b; }
			si.numC = std::max(si.numC, r.num + 1);
			if (r.rel) snprintf(b, sizeof b, "c[%d + int(a0.%s)]", r.num, compName(r.relComp).c_str());
			else snprintf(b, sizeof b, "c[%d]", r.num);
			return b;
		}
		case 3:   // vs: a0; ps: t#
			if (si.isVs) { usesA0 = true; return "a0"; }
			inputsUsed.insert(100 + r.num);
			snprintf(b, sizeof b, "t%d", r.num);
			return b;
		case 4:   // rastout
			outsWritten.insert(r.num);
			return r.num == 0 ? "oPos" : r.num == 1 ? "oFog" : "oPts";
		case 5: outsWritten.insert(16 + r.num); snprintf(b, sizeof b, "oD%d", r.num); return b;
		case 6:
			if (si.isVs && si.major < 3) { outsWritten.insert(32 + r.num); snprintf(b, sizeof b, "oT%d", r.num); return b; }
			outsWritten.insert(64 + r.num);
			snprintf(b, sizeof b, "o%d", r.num);
			return b;
		case 7:
			if (defI.count(r.num)) { snprintf(b, sizeof b, "di%d", r.num); return b; }
			snprintf(b, sizeof b, "ci[%d]", r.num);
			return b;
		case 8: ocUsed.insert(r.num); snprintf(b, sizeof b, "oC%d", r.num); return b;
		case 9: si.writesDepth = true; return "oDepth";
		case 10: snprintf(b, sizeof b, "s%d", r.num); return b;
		case 14:
			if (defB.count(r.num)) return defB[r.num] ? "true" : "false";
			snprintf(b, sizeof b, "cb[%d]", r.num);
			return b;
		case 17:
			if (r.num == 0) { si.usesFragCoord = true; return "vPos"; }
			si.usesFace = true;
			return "vFace";
		default: break;
		}
		snprintf(b, sizeof b, "BADREG_%d_%d", r.type, r.num);
		(void)dst;
		return b;
	}

	// parse one source parameter at w[p]; returns GLSL vec4 expression (modifiers + swizzle applied)
	bool parseSrc(size_t& p, std::string& e, int* sw4 = nullptr)
	{
		if (p >= nw) return fail("truncated");
		uint32_t t = w[p++];
		Reg r;
		r.tok = t;
		r.type = regType(t);
		r.num = t & 0x7ff;
		r.rel = (t >> 13) & 1;
		if (r.rel)
		{
			if (p >= nw) return fail("truncated rel");
			uint32_t a = w[p++];
			r.relComp = (a >> 16) & 3;   // swizzle x of address token
			if (regType(a) == 15) r.relComp = 0;
		}
		std::string base = regName(r, false);
		int swz = (t >> 16) & 0xff;
		std::string sw;
		for (int i = 0; i < 4; i++) sw += compName((swz >> (2 * i)) & 3);
		if (sw4) *sw4 = swz;
		std::string s = base;
		bool isIntOrBool = r.type == 7 || r.type == 14;
		if (r.type == 14) { e = "float(" + base + ")"; e = "vec4(" + e + ")"; return true; }
		if (r.type == 7) s = "vec4(" + base + ")";
		else if (r.type == 17 && r.num == 1) s = "vec4(vFace)";
		if (sw != "xyzw") s = "(" + s + ")." + sw;
		else if (isIntOrBool) {}
		int mod = (t >> 24) & 0xf;
		switch (mod)
		{
		case 0: break;
		case 1: s = "(-" + s + ")"; break;
		case 2: s = "(" + s + " - 0.5)"; break;
		case 3: s = "(-(" + s + " - 0.5))"; break;
		case 4: s = "(2.0 * (" + s + " - 0.5))"; break;
		case 5: s = "(-2.0 * (" + s + " - 0.5))"; break;
		case 6: s = "(1.0 - " + s + ")"; break;
		case 7: s = "(2.0 * " + s + ")"; break;
		case 8: s = "(-2.0 * " + s + ")"; break;
		case 9: s = "(" + s + " / " + s + ".zzzz)"; break;
		case 10: s = "(" + s + " / " + s + ".wwww)"; break;
		case 11: s = "abs(" + s + ")"; break;
		case 12: s = "(-abs(" + s + "))"; break;
		case 13: s = "vec4(!bool(" + s + ".x))"; break;
		default: return fail("src modifier");
		}
		e = s;
		return true;
	}

	struct Dst
	{
		std::string name;
		std::string mask;   // "xyzw" subset
		bool sat = false;
		int shift = 0;
		bool isA0 = false;
		bool isScalarOut = false;
	};
	bool parseDst(size_t& p, Dst& d)
	{
		if (p >= nw) return fail("truncated dst");
		uint32_t t = w[p++];
		Reg r;
		r.type = regType(t);
		r.num = t & 0x7ff;
		if ((t >> 13) & 1) return fail("relative dst");
		d.name = regName(r, true);
		int m = (t >> 16) & 15;
		for (int i = 0; i < 4; i++) if (m >> i & 1) d.mask += "xyzw"[i];
		int mods = (t >> 20) & 15;
		d.sat = mods & 1;
		int sh = (t >> 24) & 15;
		d.shift = sh >= 8 ? sh - 16 : sh;
		d.isA0 = si.isVs && r.type == 3;
		return true;
	}

	std::string wrap(const Dst& d, std::string rhs)
	{
		if (d.shift > 0) rhs = "(" + rhs + " * " + fmtFloat((float)(1 << d.shift)) + ")";
		else if (d.shift < 0) rhs = "(" + rhs + " / " + fmtFloat((float)(1 << -d.shift)) + ")";
		if (d.sat) rhs = "clamp(" + rhs + ", 0.0, 1.0)";
		return rhs;
	}
	void assign(const Dst& d, const std::string& rhs0)
	{
		std::string rhs = wrap(d, rhs0);
		if (d.mask.empty()) return;
		if (d.isA0) { line("a0." + d.mask + " = ivec4(floor(" + rhs + " + 0.5))." + d.mask + ";"); return; }
		if (d.mask == "xyzw") line(d.name + " = " + rhs + ";");
		else line(d.name + "." + d.mask + " = (" + rhs + ")." + d.mask + ";");
	}

	bool run()
	{
		if (nw < 1) return fail("empty");
		uint32_t ver = w[0];
		si.isVs = (ver >> 16) == 0xfffe;
		if (!si.isVs && (ver >> 16) != 0xffff) return fail("bad version token");
		si.major = (ver >> 8) & 0xff;
		si.minor = ver & 0xff;
		if (si.major < 2 || si.major > 3) return fail("unsupported shader model");
		vs3 = si.isVs && si.major >= 3;
		size_t p = 1;
		std::vector<std::pair<int, int>> repStack;
		int ifDepth = 0;
		while (p < nw)
		{
			uint32_t tok = w[p];
			int op = tok & 0xffff;
			if (op == 0xffff) break;
			if (op == 0xfffe)
			{
				size_t len = (tok >> 16) & 0x7fff;
				if (p + 1 < nw && w[p + 1] == 0x42415443 /* CTAB */ && p + 2 + len <= nw + 1)
					parseCtab((const uint8_t*)(w + p + 2), (len - 1) * 4, si.ctab);
				p += 1 + len;
				continue;
			}
			int len = (tok >> 24) & 15;
			int ctrl = (tok >> 16) & 0xff;
			size_t q = p + 1;
			size_t end = p + 1 + len;
			if (end > nw) return fail("instruction overruns");
			Dst d;
			std::string a, b, c, e;
			auto S = [&](std::string& dst) { return parseSrc(q, dst); };
			switch (op)
			{
			case 0: break;   // nop
			case 31:         // dcl
			{
				uint32_t u = w[q++];
				uint32_t dt = w[q++];
				Reg r;
				r.type = regType(dt);
				r.num = dt & 0x7ff;
				int usage = u & 31, idx = (u >> 16) & 15, stype = (u >> 27) & 15;
				if (r.type == 10) { if (r.num < 16) si.samplerType[r.num] = stype; }
				else if (si.isVs && r.type == 1) inDcl[r.num] = {usage, idx};
				else if (si.isVs && r.type == 6) outDcl[r.num] = {usage, idx};
				else if (!si.isVs && (r.type == 1 || r.type == 3)) psTexDcl[r.type * 1000 + r.num] = {usage, idx};
				break;
			}
			case 81:   // def
			{
				uint32_t dt = w[q++];
				int num = dt & 0x7ff;
				float f[4];
				memcpy(f, w + q, 16);
				q += 4;
				defC[num] = "vec4(" + fmtFloat(f[0]) + ", " + fmtFloat(f[1]) + ", " + fmtFloat(f[2]) + ", " + fmtFloat(f[3]) + ")";
				break;
			}
			case 48:   // defi
			{
				uint32_t dt = w[q++];
				int num = dt & 0x7ff;
				int32_t v[4];
				memcpy(v, w + q, 16);
				q += 4;
				char buf[128];
				snprintf(buf, sizeof buf, "ivec4(%d, %d, %d, %d)", v[0], v[1], v[2], v[3]);
				defI[num] = buf;
				break;
			}
			case 47:   // defb
			{
				uint32_t dt = w[q++];
				defB[dt & 0x7ff] = w[q++] != 0;
				break;
			}
			case 1: if (!parseDst(q, d) || !S(a)) return false; assign(d, a); break;
			case 2: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, a + " + " + b); break;
			case 3: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, a + " - " + b); break;
			case 4: if (!parseDst(q, d) || !S(a) || !S(b) || !S(c)) return false; assign(d, a + " * " + b + " + " + c); break;
			case 5: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, a + " * " + b); break;
			case 6: if (!parseDst(q, d) || !S(a)) return false; assign(d, "vec4(1.0 / (" + a + ").x)"); break;
			case 7: if (!parseDst(q, d) || !S(a)) return false; assign(d, "vec4(inversesqrt(abs((" + a + ").x)))"); break;
			case 8: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(dot((" + a + ").xyz, (" + b + ").xyz))"); break;
			case 9: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(dot(" + a + ", " + b + "))"); break;
			case 10: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "min(" + a + ", " + b + ")"); break;
			case 11: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "max(" + a + ", " + b + ")"); break;
			case 12: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(lessThan(" + a + ", " + b + "))"); break;
			case 13: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(greaterThanEqual(" + a + ", " + b + "))"); break;
			case 14: case 78: if (!parseDst(q, d) || !S(a)) return false; assign(d, "vec4(exp2((" + a + ").x))"); break;
			case 15: case 79: if (!parseDst(q, d) || !S(a)) return false; assign(d, "vec4(log2(abs((" + a + ").x)))"); break;
			case 16:   // lit
				if (!parseDst(q, d) || !S(a)) return false;
				assign(d, "d3d_lit(" + a + ")");
				break;
			case 18: if (!parseDst(q, d) || !S(a) || !S(b) || !S(c)) return false; assign(d, "mix(" + c + ", " + b + ", " + a + ")"); break;
			case 19: if (!parseDst(q, d) || !S(a)) return false; assign(d, "fract(" + a + ")"); break;
			case 32: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(d3d_pow((" + a + ").x, (" + b + ").x))"); break;
			case 33: if (!parseDst(q, d) || !S(a) || !S(b)) return false; assign(d, "vec4(cross((" + a + ").xyz, (" + b + ").xyz), 0.0)"); break;
			case 35: if (!parseDst(q, d) || !S(a)) return false; assign(d, "abs(" + a + ")"); break;
			case 36: if (!parseDst(q, d) || !S(a)) return false; assign(d, "(" + a + ") * inversesqrt(max(dot((" + a + ").xyz, (" + a + ").xyz), 1e-30))"); break;
			case 37:
				if (!parseDst(q, d) || !S(a)) return false;
				while (q < end) { std::string skip; if (!S(skip)) return false; }
				assign(d, "vec4(cos((" + a + ").x), sin((" + a + ").x), 0.0, 0.0)");
				break;
			case 46:   // mova
				if (!parseDst(q, d) || !S(a)) return false;
				assign(d, a);
				break;
			case 88:   // cmp
				if (!parseDst(q, d) || !S(a) || !S(b) || !S(c)) return false;
				assign(d, "mix(" + c + ", " + b + ", greaterThanEqual(" + a + ", vec4(0.0)))");
				break;
			case 90:   // dp2add
				if (!parseDst(q, d) || !S(a) || !S(b) || !S(c)) return false;
				assign(d, "vec4(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x)");
				break;
			case 66:   // texld
			case 95:   // texldl
			{
				if (!parseDst(q, d) || !S(a)) return false;
				if (q >= nw) return fail("trunc tex");
				uint32_t st = w[q++];
				int sn = st & 0x7ff;
				if (sn >= 16) return fail("sampler idx");
				int ty = si.samplerType[sn];
				char sb[16];
				snprintf(sb, sizeof sb, "s%d", sn);
				std::string rhs;
				std::string co = a;
				if (op == 95)
				{
					if (ty == 3 || ty == 4) rhs = std::string("textureLod(") + sb + ", (" + co + ").xyz, (" + co + ").w)";
					else rhs = std::string("textureLod(") + sb + ", (" + co + ").xy, (" + co + ").w)";
				}
				else
				{
					int mode = ctrl & 3;
					if (ty == 3 || ty == 4) rhs = std::string("texture(") + sb + ", (" + co + ").xyz" + (mode == 2 ? ", (" + co + ").w)" : ")");
					else if (mode == 1) rhs = std::string("texture(") + sb + ", (" + co + ").xy / (" + co + ").w)";
					else if (mode == 2) rhs = std::string("texture(") + sb + ", (" + co + ").xy, (" + co + ").w)";
					else rhs = std::string("texture(") + sb + ", (" + co + ").xy)";
				}
				assign(d, rhs);
				break;
			}
			case 65:   // texkill
			{
				Dst k;
				if (!parseDst(q, k)) return false;
				line("if (any(lessThan(" + k.name + ".xyz, vec3(0.0)))) discard;");
				break;
			}
			case 91: if (!parseDst(q, d) || !S(a)) return false; assign(d, "dFdx(" + a + ")"); break;
			case 92: if (!parseDst(q, d) || !S(a)) return false; assign(d, "dFdy(" + a + ")"); break;
			case 38:   // rep
			{
				if (!S(a)) return false;
				char buf[96];
				snprintf(buf, sizeof buf, "for (int rep%d = 0; rep%d < int((%s).x); rep%d++) {", repN, repN, a.c_str(), repN);
				repN++;
				line(buf);
				indent++;
				break;
			}
			case 39: indent--; line("}"); break;
			case 40:   // if bN
			{
				if (!S(a)) return false;
				line("if (bool((" + a + ").x)) {");
				indent++;
				ifDepth++;
				break;
			}
			case 42: indent--; line("} else {"); indent++; break;
			case 43: indent--; line("}"); ifDepth--; break;
			default:
			{
				char buf[64];
				snprintf(buf, sizeof buf, "unsupported opcode %d", op);
				return fail(buf);
			}
			}
			p = end;
		}
		return true;
	}
};

inline const char* kHelpers =
	"vec4 d3d_lit(vec4 s) {\n"
	"  float x = max(s.x, 0.0), y = max(s.y, 0.0);\n"
	"  float p = clamp(s.w, -127.9961, 127.9961);\n"
	"  float z = (s.x > 0.0 && s.y > 0.0) ? pow(s.y, p) : 0.0;\n"
	"  return vec4(1.0, x, z, 1.0);\n"
	"}\n"
	"float d3d_pow(float a, float b) { return pow(abs(a), b); }\n";

// Translate. Fills si.decls (everything before main) and si.body (inside main) -- see compose().
inline bool translate(const uint8_t* bytes, size_t n, ShaderInfo& si)
{
	si = ShaderInfo();
	if (n < 8 || (n & 3)) { si.err = "bad size"; return false; }
	std::vector<uint32_t> words(n / 4);
	memcpy(words.data(), bytes, n);
	Translator t(si, words.data(), words.size());
	if (!t.run()) return false;

	std::string decl;
	char buf[256];
	// uniforms
	snprintf(buf, sizeof buf, "uniform vec4 c[%d];\nuniform ivec4 ci[16];\nuniform bool cb[16];\n", si.isVs ? 256 : 224);
	decl += buf;
	if (si.isVs) decl += "uniform float u_flip;\n";
	for (int i = 0; i < 16; i++)
		if (si.samplerType[i])
		{
			const char* tn = si.samplerType[i] == 3 ? "samplerCube" : si.samplerType[i] == 4 ? "sampler3D" : "sampler2D";
			snprintf(buf, sizeof buf, "uniform %s s%d;\n", tn, i);
			decl += buf;
		}
	std::string pre;   // local declarations at the top of main
	for (auto& kv : t.defC) pre += "  const vec4 d" + std::to_string(kv.first) + " = " + kv.second + ";\n";
	if (!t.temps.empty())
	{
		pre += "  vec4 ";
		bool first = true;
		for (int r : t.temps) { pre += (first ? "" : ", ") + std::string("r") + std::to_string(r) + " = vec4(0.0)"; first = false; }
		pre += ";\n";
	}
	if (t.usesA0) pre += "  ivec4 a0 = ivec4(0);\n";
	for (auto& kv : t.defI) pre += "  const ivec4 di" + std::to_string(kv.first) + " = " + kv.second + ";\n";
	// ---- inputs
	if (si.isVs)
	{
		for (int r : t.inputsUsed)
		{
			auto it = t.inDcl.find(r);
			int usage = 0, idx = r;
			if (it != t.inDcl.end()) { usage = it->second.first; idx = it->second.second; }
			else { si.err = "VS input without dcl"; return false; }
			int loc = attrLocation(usage, idx);
			if (loc < 0) { loc = 15; }
			std::string nm = std::string("a_") + kUsage[usage > 13 ? 0 : usage] + std::to_string(idx);
			snprintf(buf, sizeof buf, "layout(location = %d) in vec4 %s;\n", loc, nm.c_str());
			decl += buf;
			si.inputs[nm] = loc;
			pre += "  vec4 i" + std::to_string(r) + " = " + nm + ";\n";
		}
		// outputs
		std::string post;
		auto outVar = [&](const std::string& var, const std::string& src) {
			si.outputs.insert(var);
			decl += "out vec4 " + var + ";\n";
			post += "  " + var + " = " + src + ";\n";
		};
		pre += "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n  vec4 oFog = vec4(0.0), oPts = vec4(1.0);\n";
		if (si.major < 3)
		{
			for (int k : t.outsWritten)
			{
				if (k >= 16 && k < 32) { pre += "  vec4 oD" + std::to_string(k - 16) + " = vec4(0.0);\n"; outVar("v_color" + std::to_string(k - 16), "oD" + std::to_string(k - 16)); }
				else if (k >= 32 && k < 64) { pre += "  vec4 oT" + std::to_string(k - 32) + " = vec4(0.0);\n"; outVar("v_texcoord" + std::to_string(k - 32), "oT" + std::to_string(k - 32)); }
			}
			if (t.outsWritten.count(1)) outVar("v_fog", "oFog");
		}
		else
		{
			for (int k : t.outsWritten)
				if (k >= 64)
				{
					int r = k - 64;
					auto it = t.outDcl.find(r);
					if (it == t.outDcl.end()) { si.err = "VS output without dcl"; return false; }
					int usage = it->second.first, idx = it->second.second;
					pre += "  vec4 o" + std::to_string(r) + " = vec4(0.0);\n";
					if (usage == 0) continue;   // position handled below
					if (usage == 4) continue;   // psize
					std::string nm = usage == 11 ? "v_fog" : std::string("v_") + kUsage[usage > 13 ? 0 : usage] + std::to_string(idx);
					outVar(nm, "o" + std::to_string(r));
				}
			for (auto& kv : t.outDcl)
			{
				if (kv.second.first == 0 && t.outsWritten.count(64 + kv.first)) post = "  oPos = o" + std::to_string(kv.first) + ";\n" + post;
				if (kv.second.first == 4 && t.outsWritten.count(64 + kv.first)) post = "  oPts = o" + std::to_string(kv.first) + ";\n" + post;
			}
		}
		post += "  gl_Position = vec4(oPos.x, oPos.y * u_flip, 2.0 * oPos.z - oPos.w, oPos.w);\n  gl_PointSize = oPts.x;\n";
		si.decls = decl + kHelpers;
		si.body = pre + t.out + post;
	}
	else
	{
		// PS inputs: ps2: v0,v1 colors, t# texcoords. ps3: dcl'd regs.
		std::string names;
		for (int r : t.inputsUsed)
		{
			std::string nm;
			std::string local;
			if (r >= 100) { nm = "v_texcoord" + std::to_string(r - 100); local = "t" + std::to_string(r - 100); }
			else if (si.major < 3) { nm = "v_color" + std::to_string(r); local = "i" + std::to_string(r); }
			else
			{
				auto it = t.psTexDcl.find(1000 + r);
				if (it == t.psTexDcl.end()) { si.err = "PS input without dcl"; return false; }
				int usage = it->second.first, idx = it->second.second;
				nm = usage == 11 ? "v_fog" : std::string("v_") + kUsage[usage > 13 ? 0 : usage] + std::to_string(idx);
				local = "i" + std::to_string(r);
			}
			si.psIns.push_back(nm);
			pre += "  vec4 " + local + " = " + nm + ";\n";
		}
		if (si.usesFragCoord) pre += "  vec4 vPos = vec4(gl_FragCoord.xy, 0.0, 0.0);\n";
		if (si.usesFace) pre += "  float vFace = gl_FrontFacing ? 1.0 : -1.0;\n";
		for (int r : t.ocUsed)
		{
			decl += "layout(location = " + std::to_string(r) + ") out vec4 o_C" + std::to_string(r) + ";\n";
			pre += "  vec4 oC" + std::to_string(r) + " = vec4(0.0);\n";
			si.psOutputs |= 1 << r;
		}
		std::string post;
		for (int r : t.ocUsed) post += "  o_C" + std::to_string(r) + " = oC" + std::to_string(r) + ";\n";
		if (si.writesDepth) { pre += "  vec4 oDepth = vec4(0.0);\n"; post += "  gl_FragDepth = oDepth.x;\n"; }
		si.decls = decl + kHelpers;
		si.body = pre + t.out + post;
	}
	si.ok = true;
	return true;
}

// Complete GLSL source. For a pixel shader, `vsOuts` lists the varyings the matching vertex shader writes; the others
// are declared as plain zero-initialised globals so that the program links.
inline std::string compose(const ShaderInfo& si, const std::set<std::string>* vsOuts = nullptr)
{
	std::string s = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler3D;\n";
	s += si.decls;
	if (!si.isVs)
	{
		std::set<std::string> done;
		for (auto& n : si.psIns)
		{
			if (!done.insert(n).second) continue;
			if (!vsOuts || vsOuts->count(n)) s += "in vec4 " + n + ";\n";
			else s += "vec4 " + n + " = " + (n == "v_color0" || n == "v_color1" ? "vec4(1.0)" : "vec4(0.0)") + ";\n";
		}
	}
	s += "void main() {\n" + si.body + "}\n";
	return s;
}
}   // namespace shdr
}   // namespace d3d
