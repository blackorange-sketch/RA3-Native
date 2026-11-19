#pragma once
// DXT → ETC2 transcoding for GPUs without S3TC (Mali, PowerVR), via the vendored etcpak encoder.
#include <cstdint>
#include <vector>

namespace etc2
{
constexpr uint32_t kRgb8 = 0x9274;        // GL_COMPRESSED_RGB8_ETC2
constexpr uint32_t kRgba8 = 0x9278;       // GL_COMPRESSED_RGBA8_ETC2_EAC
inline uint32_t levelBytes(uint32_t w, uint32_t h, bool alpha) { return ((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16u : 8u); }
// rgba: w*h pixels, bytes R,G,B,A. Appends the ETC2 (alpha: RGBA8_EAC, else RGB8) data of the image to out.
void encode(const uint8_t* rgba, uint32_t w, uint32_t h, bool alpha, std::vector<uint8_t>& out);
}
