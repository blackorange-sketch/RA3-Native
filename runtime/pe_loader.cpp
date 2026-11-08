#include "pe_loader.h"

#include <cstdio>
#include <cstring>

#include "guest.h"
#include "imports.h"
#include "cpu.h"

namespace pe
{
namespace
{
std::vector<uint8_t> file;

template <class T> T rd(size_t off) { T v{}; if (off + sizeof v <= file.size()) memcpy(&v, &file[off], sizeof v); return v; }

uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
}

bool load(const char* path, Image& img, std::string& err)
{
	FILE* f = fopen(path, "rb");
	if (!f) { err = std::string("cannot open ") + path; return false; }
	fseek(f, 0, SEEK_END);
	file.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(file.data(), 1, file.size(), f) != file.size()) { fclose(f); err = "short read"; return false; }
	fclose(f);

	if (rd<uint16_t>(0) != 0x5a4d) { err = "not an MZ file"; return false; }
	uint32_t pe = rd<uint32_t>(0x3c);
	if (rd<uint32_t>(pe) != 0x4550 || rd<uint16_t>(pe + 4) != 0x14c) { err = "not a 32-bit x86 PE"; return false; }
	uint16_t nsec = rd<uint16_t>(pe + 6), optSize = rd<uint16_t>(pe + 20);
	size_t opt = pe + 24;
	if (rd<uint16_t>(opt) != 0x10b) { err = "not PE32"; return false; }

	img.entry = rd<uint32_t>(opt + 16);
	img.base = rd<uint32_t>(opt + 28);
	img.sizeOfImage = rd<uint32_t>(opt + 56);
	uint32_t hdrSize = rd<uint32_t>(opt + 60);
	auto dir = [&](int i, uint32_t& rva, uint32_t& size) {
		rva = rd<uint32_t>(opt + 96 + i * 8);
		size = rd<uint32_t>(opt + 96 + i * 8 + 4);
	};

	if (!guest::init()) { err = "address-space reservation failed"; return false; }

	// Headers
	if (!guest::commit(img.base, align(hdrSize, 0x1000))) { err = "commit headers"; return false; }
	memcpy(guest::host(img.base), file.data(), std::min<size_t>(hdrSize, file.size()));

	// Sections
	size_t sec = opt + optSize;
	for (int i = 0; i < nsec; i++)
	{
		size_t o = sec + i * 40;
		Section s;
		s.name.assign(reinterpret_cast<const char*>(&file[o]), strnlen(reinterpret_cast<const char*>(&file[o]), 8));
		uint32_t vsize = rd<uint32_t>(o + 8), va = rd<uint32_t>(o + 12), rsize = rd<uint32_t>(o + 16), raw = rd<uint32_t>(o + 20);
		s.va = va; s.vsize = vsize; s.flags = rd<uint32_t>(o + 36);
		uint32_t span = align(std::max(vsize, rsize), 0x1000);
		if (!guest::commit(img.base + va, span)) { err = "commit section " + s.name; return false; }
		if (raw + rsize > file.size()) { err = "section outside file: " + s.name; return false; }
		memcpy(guest::host(img.base + va), &file[raw], std::min(rsize, vsize ? vsize : rsize));
		img.sections.push_back(s);
	}

	// Imports: fill each IAT slot with a stub address.
	uint32_t impRva, impSize;
	dir(1, impRva, impSize);
	for (uint32_t d = impRva; impRva; d += 20)
	{
		uint32_t oft = guest::load<uint32_t>(img.base + d), nameRva = guest::load<uint32_t>(img.base + d + 12),
			first = guest::load<uint32_t>(img.base + d + 16);
		if (!nameRva) break;
		std::string dll(reinterpret_cast<const char*>(guest::host(img.base + nameRva)));
		uint32_t lookup = oft ? oft : first;
		for (uint32_t i = 0;; i++)
		{
			uint32_t t = guest::load<uint32_t>(img.base + lookup + i * 4);
			if (!t) break;
			std::string name = (t & 0x80000000) ? "#" + std::to_string(t & 0xffff)
				: std::string(reinterpret_cast<const char*>(guest::host(img.base + t + 2)));
			uint32_t stub = imports::bind(dll, name);
			if (name == "tolower") ra3_stub_tolower = stub;
			guest::store<uint32_t>(img.base + first + i * 4, stub);
			img.importCount++;
		}
	}

	// TLS directory (VAs, not RVAs, because there are no relocations to adjust).
	uint32_t tlsRva, tlsSize;
	dir(9, tlsRva, tlsSize);
	if (tlsRva)
	{
		uint32_t a = img.base + tlsRva;
		img.tlsStart = guest::load<uint32_t>(a);
		img.tlsEnd = guest::load<uint32_t>(a + 4);
		img.tlsIndexAddr = guest::load<uint32_t>(a + 8);
		img.tlsCallbacks = guest::load<uint32_t>(a + 12);
		img.tlsZeroFill = guest::load<uint32_t>(a + 16);
		if (img.tlsIndexAddr)
			guest::store<uint32_t>(img.tlsIndexAddr, 0);   // slot 0 is reserved for the image's own TLS block
	}

	return true;
}
}
