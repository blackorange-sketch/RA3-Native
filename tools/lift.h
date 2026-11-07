// x86-32 -> C lifter (integer subset first). Pure library: bytes in, C text out.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lift
{
struct Reader
{
	virtual ~Reader() = default;
	virtual bool read(uint32_t va, uint8_t* out, size_t n) = 0;   // false when va is not mapped
	virtual bool isCode(uint32_t va) = 0;
};

struct Result
{
	std::string code;                       // C source of the function, "f_<va>"
	size_t instructions = 0;
	std::vector<std::string> unsupported;   // "va mnemonic operands", one per unlifted instruction
};

struct Options
{
	std::set<uint32_t> setjmpThunks;                   // VAs of `jmp [setjmp slot]` stubs: direct calls to them are setjmp sites too
	uint32_t setjmpSlot = 0;                           // VA of the IAT slot of _setjmp3 (0 = none): calls through it get a longjmp landing pad
	const std::set<uint32_t>* funcEntries = nullptr;   // known function starts: jumps to them are tail calls
};

// Lifts the function starting at `entry`. The generated C needs runtime/cpu.h.
Result liftFunction(Reader& r, uint32_t entry, const Options& opt);

// Opens/closes the shared Capstone handle (call once).
void init();
}
