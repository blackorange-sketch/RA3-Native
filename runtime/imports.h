// Import binding. Every imported function gets a stub address in the guest window; the IAT holds that
// address. Recompiled `call [iat]` resolves the stub address to a host function through lookup().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CPU;

namespace imports
{
// A host implementation reads its arguments from the guest stack ([esp+4+4*i]; [esp] is the return
// address) and leaves the result in eax (st0 for floating-point results).
using HostFn = void (*)(CPU*);

struct Entry
{
	std::string dll, name;   // name is "#<ordinal>" for ordinal imports
	uint32_t stub;           // guest address of the stub
	HostFn fn;               // nullptr until implemented
	int pop;                 // bytes of arguments the callee removes (stdcall); 0 for cdecl
	uint64_t calls = 0;
};

constexpr uint32_t StubBase = 0x70000000;   // above any section, below the stack
constexpr uint32_t StubStride = 16;

void registerFn(const char* dll, const char* name, HostFn fn, int pop);
// Data imports (variables exported by a DLL): the IAT slot must hold the variable's address.
using DataInit = uint32_t (*)();
void registerData(const char* dll, const char* name, uint32_t size, DataInit init);
extern uint32_t (*dataAlloc)(uint32_t size);
uint32_t bind(const std::string& dll, const std::string& name);   // returns stub address
Entry* lookup(uint32_t stubAddr);
const std::vector<Entry>& all();
}
