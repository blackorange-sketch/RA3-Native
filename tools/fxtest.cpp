// Host test for the effect parser/runtime helpers: runs over unpacked .fxo files.
#include "../runtime/d3d9_fxrt.h"
#include <fstream>
#include <iterator>
using namespace d3d;
int main(int argc, char** argv)
{
	int nfx = 0, bad = 0, nsh = 0, miss = 0, maxArr = 0;
	for (int i = 1; i < argc; i++)
	{
		std::ifstream f(argv[i], std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
		FxInfo fx;
		if (!parseEffect(d.data(), d.size(), fx)) { printf("PARSE FAIL %s\n", argv[i]); bad++; continue; }
		nfx++;
		fxrt::initParams(fx);
		for (size_t t = 0; t < fx.techs.size(); t++)
			for (size_t p = 0; p < fx.techs[t].passes.size(); p++)
				for (size_t s = 0; s < fx.techs[t].passes[p].sts.size(); s++)
				{
					auto& st = fx.techs[t].passes[p].sts[s];
					auto si = fxrt::stateInfo(st.op);
					if (si.cls != fxrt::SC_VS && si.cls != fxrt::SC_PS) continue;
					fxrt::ShaderRef r;
					if (!fxrt::resolveShader(fx, (uint32_t)t, (uint32_t)p, (uint32_t)s, r)) { printf("no shader %s tech %zu pass %zu state %zu\n", argv[i], t, p, s); miss++; continue; }
					ShaderInfo sh;
					if (!shdr::translate(r.code, r.size, sh)) { printf("translate fail %s: %s\n", argv[i], sh.err.c_str()); bad++; }
					if (sh.isVs != (si.cls == fxrt::SC_VS)) { printf("stage mismatch %s\n", argv[i]); bad++; }
					nsh++;
					maxArr = std::max(maxArr, r.arrayIndex);
				}
	}
	printf("effects %d, shaders resolved %d, missing %d, bad %d, max array index %d\n", nfx, nsh, miss, bad, maxArr);
}
