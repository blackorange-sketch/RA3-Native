// Texture data helpers, see d3d9_tex.h (moved out of the OpenGL renderer, unchanged)
#include <cstdio>
#include <cstring>
#include "d3d9.h"
#include "d3d9_tex.h"
namespace d3d
{
namespace tex
{
// ---- software DXT1/3/5 decoder for GPUs without GL_EXT_texture_compression_s3tc (Mali, PowerVR)
void dxtColors(const uint8_t* b, uint8_t c[4][4], bool dxt1)
{
	uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	auto rgb = [](uint16_t v, uint8_t* d) { d[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); d[2] = (uint8_t)((v & 31) * 255 / 31); d[3] = 255; };
	rgb(c0, c[0]); rgb(c1, c[1]);
	if (!dxt1 || c0 > c1)
		for (int k = 0; k < 3; k++) { c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3); c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3); }
	else
		for (int k = 0; k < 3; k++) { c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2); c[3][k] = 0; }
	c[2][3] = 255; c[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
}
void decodeDxt(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	out.assign((size_t)w * h * 4, 0);
	bool dxt1 = fmt == 0x31545844, dxt3 = fmt == 0x33545844;
	uint32_t bwk = (w + 3) / 4, bhk = (h + 3) / 4, bs = dxt1 ? 8 : 16;
	for (uint32_t by = 0; by < bhk; by++)
		for (uint32_t bx = 0; bx < bwk; bx++)
		{
			const uint8_t* b = src + ((size_t)by * bwk + bx) * bs;
			uint8_t a[16];
			for (int i = 0; i < 16; i++) a[i] = 255;
			if (dxt3) for (int i = 0; i < 16; i++) a[i] = (uint8_t)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
			else if (!dxt1)
			{
				uint8_t a0 = b[0], a1 = b[1], t[8] = {a0, a1};
				if (a0 > a1) for (int k = 1; k < 7; k++) t[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1) / 7);
				else { for (int k = 1; k < 5; k++) t[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1) / 5); t[6] = 0; t[7] = 255; }
				uint64_t bits = 0; for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
				for (int i = 0; i < 16; i++) a[i] = t[(bits >> (3 * i)) & 7];
			}
			const uint8_t* cb = dxt1 ? b : b + 8;
			uint8_t c[4][4];
			dxtColors(cb, c, dxt1);
			uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
			for (int i = 0; i < 16; i++)
			{
				uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
				if (x >= w || y >= h) continue;
				const uint8_t* cc = c[(idx >> (2 * i)) & 3];
				uint8_t* d = &out[((size_t)y * w + x) * 4];
				d[0] = cc[0]; d[1] = cc[1]; d[2] = cc[2]; d[3] = dxt1 ? cc[3] : a[i];
			}
		}
}

// expands a level of a "convert" format to RGBA8
void expandLevel(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	if (fmt == 0x31545844 || fmt == 0x33545844 || fmt == 0x35545844) { decodeDxt(fmt, src, w, h, out); return; }
	out.resize((size_t)w * h * 4);
	uint32_t pitch = w * bpp(fmt);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			const uint8_t* s = src + (size_t)y * pitch + (size_t)x * bpp(fmt);
			uint8_t* d = &out[((size_t)y * w + x) * 4];
			switch (fmt)
			{
			case 21: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
			case 22: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; break;
			case 20: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; break;
			case 24: case 25:
			{
				uint16_t v = s[0] | (s[1] << 8);
				d[0] = ((v >> 10) & 31) * 255 / 31; d[1] = ((v >> 5) & 31) * 255 / 31; d[2] = (v & 31) * 255 / 31;
				d[3] = fmt == 25 ? ((v >> 15) ? 255 : 0) : 255;
				break;
			}
			case 26:
			{
				uint16_t v = s[0] | (s[1] << 8);
				d[0] = ((v >> 8) & 15) * 17; d[1] = ((v >> 4) & 15) * 17; d[2] = (v & 15) * 17; d[3] = ((v >> 12) & 15) * 17;
				break;
			}
			default: d[0] = d[1] = d[2] = 0; d[3] = 255; break;
			}
		}
}

uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t h)
{
	const uint64_t m = 0x9E3779B97F4A7C15ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * m; h ^= h >> 29; }
	for (; i < n; i++) { h = (h ^ p[i]) * m; }
	return h ^ (h >> 31);
}
bool readWhole(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}
void writeWhole(const std::string& path, const std::vector<uint8_t>& data)
{
	std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) return;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = fclose(f) == 0 && ok;
	if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
}
}
}
