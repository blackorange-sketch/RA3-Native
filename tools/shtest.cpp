// Host test: translate D3D9 shader byte code files to GLSL (for validation with a real GLSL compiler).
#include "../runtime/d3d9_sh.h"
#include <fstream>
#include <iterator>
int main(int argc, char** argv)
{
	std::string outDir = argv[1];
	int bad = 0, n = 0;
	for (int i = 2; i < argc; i++)
	{
		std::ifstream f(argv[i], std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
		d3d::ShaderInfo si;
		n++;
		if (!d3d::shdr::translate(d.data(), d.size(), si)) { printf("TRANSLATE FAIL %s: %s\n", argv[i], si.err.c_str()); bad++; continue; }
		std::string b = argv[i];
		b = b.substr(b.rfind('/') + 1);
		b = b.substr(0, b.rfind('.'));
		std::ofstream o(outDir + "/" + b + (si.isVs ? ".vert" : ".frag"));
		o << d3d::shdr::compose(si);
	}
	printf("translated %d, failed %d\n", n, bad);
}
