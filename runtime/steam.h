#pragma once
// Stand-in steam_api.dll (steam.cpp)
#include <cstdint>
#include <string>

namespace steam
{
uint32_t loadLibrary(const std::string& name);                  // module handle for steam_api.dll, else 0
uint32_t procAddress(uint32_t module, const std::string& name);  // export of that module, else 0
}
