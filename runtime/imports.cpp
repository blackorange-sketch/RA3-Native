#include "imports.h"

#include <algorithm>
#include <map>

namespace imports
{
static std::vector<Entry> entries;
static std::map<std::string, HostFn> registry;

static std::string key(std::string dll, const std::string& name)
{
	std::transform(dll.begin(), dll.end(), dll.begin(), [](unsigned char c) { return std::tolower(c); });
	return dll + "!" + name;
}

void registerFn(const char* dll, const char* name, HostFn fn) { registry[key(dll, name)] = fn; }

uint32_t bind(const std::string& dll, const std::string& name)
{
	uint32_t stub = StubBase + static_cast<uint32_t>(entries.size()) * StubStride;
	auto it = registry.find(key(dll, name));
	entries.push_back({dll, name, stub, it == registry.end() ? nullptr : it->second});
	return stub;
}

const Entry* lookup(uint32_t a)
{
	if (a < StubBase || (a - StubBase) % StubStride)
		return nullptr;

	size_t i = (a - StubBase) / StubStride;
	return i < entries.size() ? &entries[i] : nullptr;
}

const std::vector<Entry>& all() { return entries; }
}
