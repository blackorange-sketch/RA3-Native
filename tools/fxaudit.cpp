// Build: g++ -O1 -std=c++17 -Iruntime tools/fxaudit.cpp -o fxaudit
// Run:   ./fxaudit out/ path/to/*.fxo   (then: node tools/fxaudit_glcheck.js out/  -- compiles every pass in Chromium WebGL2)
// Offline audit of every effect: translate all shaders of all techniques and check how the effect runtime feeds them.
//  * translation failures (unsupported opcodes, missing declarations)
//  * constants the shader reads that nothing fills (no ctab entry, no def, no preshader output)
//  * ctab entries without a matching effect parameter
//  * parameters larger than the registers the ctab assigns (truncated) and overlapping ctab ranges
//  * samplers without a parameter / texture
//  * preshaders that fail to run
// Writes the GLSL of each pass to out/<effect>.<tech>.<pass>.{vs,fs} for a compile check.
#include "d3d9_fxrt.h"
#include <cstdarg>
#include <fstream>
#include <iterator>
#include <regex>
#include <sys/stat.h>
using namespace d3d;

static int issues = 0;
static std::map<uint32_t, std::map<uint32_t, std::set<std::string>>> smpUse;
static std::map<uint32_t, std::map<uint32_t, std::set<std::string>>> rsUse;   // rs id -> value -> effects
static void issue(const std::string& where, const char* fmt, ...)
{
	char b[512];
	va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
	printf("  [%s] %s\n", where.c_str(), b);
	issues++;
}

// registers a parameter occupies when packed from register 0
static uint32_t regsNeeded(const FxParam& p, uint32_t set)
{
	std::vector<uint32_t> dst(4 * 512, 0xdeadbeefu), src(std::max<size_t>(p.val.size(), 1), 0x3f800000u);
	fxrt::packToRegs(p.ty, src.data(), src.size(), set, 0, dst.data(), set == 0 ? 2048 : 512);
	uint32_t last = 0;
	for (uint32_t i = 0; i < dst.size(); i++) if (dst[i] != 0xdeadbeefu) last = i + 1;
	return set == 0 ? last : (last + 3) / 4;
}

int main(int argc, char** argv)
{
	std::string outDir = argv[1];
	mkdir(outDir.c_str(), 0755);
	for (int ai = 2; ai < argc; ai++)
	{
		std::string path = argv[ai];
		std::string name = path.substr(path.rfind('/') + 1);
		name = name.substr(0, name.rfind('.'));
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
		FxInfo fx;
		printf("== %s\n", name.c_str());
		if (!parseEffect(d.data(), d.size(), fx)) { issue(name, "effect does not parse"); continue; }
		fxrt::initParams(fx);
		for (auto& pp : fx.params) for (auto& sm : pp.smp) smpUse[sm.op][sm.v].insert(name);
		for (size_t t = 0; t < fx.techs.size(); t++)
			for (size_t p = 0; p < fx.techs[t].passes.size(); p++)
			{
				std::string where = name + "." + fx.techs[t].name + "." + std::to_string(p);
				ShaderInfo vsI, psI;
				bool haveVs = false, havePs = false;
				for (size_t s = 0; s < fx.techs[t].passes[p].sts.size(); s++)
				{
					auto& st = fx.techs[t].passes[p].sts[s];
					auto si = fxrt::stateInfo(st.op);
					if (si.cls == fxrt::SC_RS) rsUse[si.id][fx.u32(fx.base + st.vo)].insert(name);
					if (si.cls != fxrt::SC_VS && si.cls != fxrt::SC_PS) continue;
					fxrt::ShaderRef r;
					if (!fxrt::resolveShader(fx, (uint32_t)t, (uint32_t)p, (uint32_t)s, r)) { issue(where, "shader state %u does not resolve", st.op); continue; }
					if (!r.code || !r.size) continue;   // NULL shader (fixed function)
					ShaderInfo sh;
					if (!shdr::translate(r.code, r.size, sh)) { issue(where, "%s translate failed: %s", si.cls == fxrt::SC_VS ? "VS" : "PS", sh.err.c_str()); continue; }
					if (sh.isVs) { vsI = sh; haveVs = true; } else { psI = sh; havePs = true; }
				}
				for (int k = 0; k < 2; k++)
				{
					ShaderInfo& sh = k == 0 ? vsI : psI;
					if (!(k == 0 ? haveVs : havePs)) continue;
					const char* st = k == 0 ? "VS" : "PS";
					std::string body = sh.body;
					if (body.find("unsupported") != std::string::npos) issue(where, "%s contains unsupported instructions", st);
					// preshader outputs
					std::set<int> covered;
					if (!sh.pres.empty())
					{
						std::function<void(const CtabConst&, double*, size_t)> fill = [&](const CtabConst& c, double* dst, size_t nd) {
							if (fxrt::findParam(fx, c.name) < 0) issue(where, "%s preshader input '%s' has no parameter", st, c.name.c_str());
							for (size_t i = 0; i < nd; i++) dst[i] = 0.5;
						};
						fxrt::PresOut po;
						if (!fxrt::runPreshader(sh.pres.data(), sh.pres.size(), fill, po)) issue(where, "%s preshader fails to run", st);
						for (auto& w : po.f) covered.insert((int)(w.first / 4));
					}
					for (auto& dc : sh.defF) covered.insert(dc.first);
					// ctab checks
					std::vector<std::pair<uint32_t, uint32_t>> ranges;   // float regs
					for (auto& c : sh.ctab)
					{
						int pk = fxrt::findParam(fx, c.name);
						if (c.regSet == 3)
						{
							if (pk < 0) { issue(where, "%s sampler '%s' has no parameter", st, c.name.c_str()); continue; }
							const FxParam& sp = fx.params[pk];
							if (!sp.smpTex) issue(where, "%s sampler '%s' has no texture parameter (set directly by the game?)", st, c.name.c_str());
							continue;
						}
						if (pk < 0) { issue(where, "%s constant '%s' r%u x%u has no parameter: its registers stay stale", st, c.name.c_str(), c.regIndex, c.regCount); continue; }
						const FxParam& pp = fx.params[pk];
						if (c.regSet == 2)
						{
							uint32_t need = regsNeeded(pp, 2);
							if (need > c.regCount && c.regCount) printf("  (info) [%s] %s '%s' uses %u of %u registers\n", where.c_str(), st, c.name.c_str(), c.regCount, need);
							for (uint32_t i = 0; i < std::max(1u, c.regCount); i++) covered.insert((int)(c.regIndex + i));
							for (auto& rg : ranges)
								if (c.regIndex < rg.first + rg.second && rg.first < c.regIndex + std::max(1u, c.regCount))
									issue(where, "%s ctab ranges overlap at r%u ('%s')", st, c.regIndex, c.name.c_str());
							ranges.push_back({c.regIndex, std::max(1u, c.regCount)});
						}
					}
					// constant reads with a literal index that nothing fills
					std::regex re(k == 0 ? "vc\\[([0-9]+)\\]" : "pc\\[([0-9]+)\\]");
					std::set<int> reads;
					for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it) reads.insert(std::stoi((*it)[1]));
					for (int rr : reads) if (!covered.count(rr)) issue(where, "%s reads c%d which no constant, def or preshader fills", st, rr);
				}
				if (haveVs || havePs)
				{
					std::string base = outDir + "/" + where;
					if (haveVs) { std::ofstream o(base + ".vs"); o << shdr::compose(vsI); }
					if (havePs) { std::ofstream o(base + ".fs"); o << shdr::compose(psI, haveVs ? &vsI.outputs : nullptr); }
				}
			}
	}
	static const std::set<uint32_t> handled = {7, 14, 15, 16, 19, 20, 22, 23, 24, 25, 27, 52, 53, 54, 55, 56, 57, 58, 59, 168, 171, 174, 175, 193, 195, 206, 207, 208, 209};
	printf("render states set by effects that the renderer ignores:\n");
	for (auto& kv : rsUse)
		if (!handled.count(kv.first))
			for (auto& v : kv.second)
			{
				std::string e; int n = 0;
				for (auto& x : v.second) { if (n++ < 6) e += x + " "; }
				printf("  RS %u = %u in %zu effects: %s%s\n", kv.first, v.first, v.second.size(), e.c_str(), v.second.size() > 6 ? "..." : "");
			}
	printf("sampler states in effects (op: value -> count):\n");
	for (auto& kv : smpUse) { printf("  op %u:", kv.first); for (auto& v : kv.second) printf(" %u->%zu", v.first, v.second.size()); printf("\n"); }
	printf("TOTAL issues: %d\n", issues);
	return 0;
}
