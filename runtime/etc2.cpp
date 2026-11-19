#include "etc2.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "third_party/etcpak/ProcessRGB.hpp"

namespace etc2
{
void encode(const uint8_t* rgba, uint32_t w, uint32_t h, bool alpha, std::vector<uint8_t>& out)
{
	uint32_t pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
	// etcpak reads B,G,R,A bytes and whole 4x4 blocks: swap and pad by repeating the edge pixels
	std::vector<uint32_t> src((size_t)pw * ph);
	for (uint32_t y = 0; y < ph; y++)
	{
		const uint8_t* row = rgba + (size_t)std::min(y, h - 1) * w * 4;
		uint32_t* d = &src[(size_t)y * pw];
		for (uint32_t x = 0; x < pw; x++)
		{
			const uint8_t* p = row + (size_t)std::min(x, w - 1) * 4;
			d[x] = (uint32_t)p[2] | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16) | ((uint32_t)p[3] << 24);
		}
	}
	uint32_t bw = pw / 4, bh = ph / 4, per = alpha ? 2 : 1;   // uint64 words per block
	size_t base = out.size();
	out.resize(base + (size_t)bw * bh * per * 8);
	uint64_t* dst = (uint64_t*)(out.data() + base);
	auto run = [&](uint32_t r0, uint32_t r1) {
		if (r1 <= r0) return;
		const uint32_t* s = &src[(size_t)r0 * 4 * pw];
		uint64_t* o = dst + (size_t)r0 * bw * per;
		if (alpha) CompressEtc2Rgba(s, o, bw * (r1 - r0), pw, false);
		else CompressEtc2Rgb(s, o, bw * (r1 - r0), pw, false);
	};
	// big levels are split into horizontal bands over a few threads (first loads transcode hundreds of textures)
	uint32_t threads = bw * bh >= 4096 ? 4 : 1;
	if (threads == 1) { run(0, bh); return; }
	std::thread t[3];
	uint32_t step = (bh + threads - 1) / threads;
	for (uint32_t k = 1; k < threads; k++) t[k - 1] = std::thread(run, std::min(bh, k * step), std::min(bh, (k + 1) * step));
	run(0, std::min(bh, step));
	for (auto& th : t) th.join();
}
}
