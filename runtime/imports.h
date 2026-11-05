// Import binding. Every imported function gets a stub address in the guest window; the IAT holds that
// address. Recompiled `call [iat]` resolves the stub address to a host function through lookup().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace imports
{
using HostFn = void (*)();   // real signature is per-function; the call shim casts it

struct Entry
{
	std::string dll, name;   // name is "#<ordinal>" for ordinal imports
	uint32_t stub;           // guest address of the stub
	HostFn fn;               // nullptr until implemented
};

constexpr uint32_t StubBase = 0x70000000;   // above any section, below the stack
constexpr uint32_t StubStride = 16;

void registerFn(const char* dll, const char* name, HostFn fn);
uint32_t bind(const std::string& dll, const std::string& name);   // returns stub address
const Entry* lookup(uint32_t stubAddr);
const std::vector<Entry>& all();
}
