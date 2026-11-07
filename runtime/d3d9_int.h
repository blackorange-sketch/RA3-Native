// Internal helpers shared by the d3d9*.cpp translation units: handler registration macros.
#pragma once
#include "d3d9.h"

namespace d3d
{
std::map<std::string, Handler>& handlers();
struct FxInfo;
void fxRegister(const std::shared_ptr<FxInfo>& fx);   // d3d9_fx.cpp: shared effect parameters
}
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define H(name) static ::d3d::Reg CAT(reg_, __LINE__)(name, [](CPU* c, ::d3d::Obj* o) -> void
#define END )
