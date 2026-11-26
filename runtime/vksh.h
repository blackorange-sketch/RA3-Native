// Vulkan renderer, stage 2 (docs/VULKAN_PLAN.md): GLSL (shdr::composeVk) -> SPIR-V with glslang built into the app,
// cached on disk (<data>/spvcache/<hash>.spv) so a shader is compiled once per install.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vksh
{
bool available();   // glslang is built in (the Android build with GLSLANG_DIR)
// thread safe; false with `err` set when the source does not compile
bool compile(const std::string& glsl, bool vertex, std::vector<uint32_t>& spv, std::string& err);
// stage 2 check, Vulkan mode: every shader the game creates is translated and compiled in the background
void check(const std::vector<uint8_t>& byteCode);
}
