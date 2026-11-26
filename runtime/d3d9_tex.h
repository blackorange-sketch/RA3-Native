// Texture data helpers shared by the renderers (d3d9_tex.cpp): DXT decoding, expansion of formats a GPU cannot
// sample directly to RGBA8, and small file helpers for the texture caches.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace d3d
{
namespace tex
{
void decodeDxt(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);   // DXT1/3/5 -> RGBA8
void expandLevel(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);  // "convert" formats -> RGBA8
uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t h);
bool readWhole(const std::string& path, std::vector<uint8_t>& out);
void writeWhole(const std::string& path, const std::vector<uint8_t>& data);   // through a temporary file
}
}
