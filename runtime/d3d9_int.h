// Internal helpers shared by the d3d9*.cpp translation units: handler registration macros.
#pragma once
#include "d3d9.h"

namespace d3d
{
std::map<std::string, Handler>& handlers();
}
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define H(name) static ::d3d::Reg CAT(reg_, __LINE__)(name, [](CPU* c, ::d3d::Obj* o) -> void
#define END )
