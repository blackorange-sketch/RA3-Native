#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pe
{
struct Section { std::string name; uint32_t va, vsize, flags; };

struct Image
{
	uint32_t base = 0, entry = 0, sizeOfImage = 0;
	std::vector<Section> sections;
	// Thread-local storage directory (the game has a small .tls section).
	uint32_t tlsStart = 0, tlsEnd = 0, tlsIndexAddr = 0, tlsCallbacks = 0, tlsZeroFill = 0;
	uint32_t importCount = 0;
};

// Maps the PE into the guest window and binds all imports. Returns false and fills err on failure.
bool load(const char* path, Image& out, std::string& err);
}
