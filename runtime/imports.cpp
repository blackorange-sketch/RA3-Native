#include "imports.h"
#include "guest.h"

#include <algorithm>
#include <map>

namespace imports
{
namespace
{
struct Reg { HostFn fn; int pop; };
struct DataReg { uint32_t size; DataInit init; };
std::map<std::string, DataReg>& dataRegistry() { static std::map<std::string, DataReg> r; return r; }
std::map<std::string, Reg>& registry() { static std::map<std::string, Reg> r; return r; }
std::vector<Entry>& entries() { static std::vector<Entry> e; return e; }

std::string key(std::string dll, const std::string& name)
{
	std::transform(dll.begin(), dll.end(), dll.begin(), [](unsigned char c) { return std::tolower(c); });
	return dll + "!" + name;
}
}

uint32_t (*dataAlloc)(uint32_t) = nullptr;
void registerFn(const char* dll, const char* name, HostFn fn, int pop) { registry()[key(dll, name)] = {fn, pop}; }
void registerData(const char* dll, const char* name, uint32_t size, DataInit init) { dataRegistry()[key(dll, name)] = {size, init}; }

uint32_t bind(const std::string& dll, const std::string& name)
{
	if (auto d = dataRegistry().find(key(dll, name)); d != dataRegistry().end() && dataAlloc)
	{
		uint32_t a = dataAlloc(d->second.size);
		if (d->second.init) *reinterpret_cast<uint32_t*>(guest::base + a) = d->second.init();
		return a;
	}
	uint32_t stub = StubBase + static_cast<uint32_t>(entries().size()) * StubStride;
	auto it = registry().find(key(dll, name));
	entries().push_back({dll, name, stub, it == registry().end() ? nullptr : it->second.fn, it == registry().end() ? 0 : it->second.pop});
	return stub;
}

Entry* lookup(uint32_t a)
{
	if (a < StubBase || (a - StubBase) % StubStride)
		return nullptr;

	size_t i = (a - StubBase) / StubStride;
	return i < entries().size() ? &entries()[i] : nullptr;
}

const std::vector<Entry>& all() { return entries(); }
}
