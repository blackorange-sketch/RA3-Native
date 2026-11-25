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

// What a function does with the guest registers, over all its paths and callees (bit i: eax ecx edx ebx esp ebp
// esi edi): R - registers whose value at entry it may read, W - registers whose value at return may differ from
// the one at entry (callee-saved registers it pushes in its prologue and pops before every exit are not in W).
// A direct call then hands over only R and takes back only W instead of all eight registers.
struct RegSum { uint8_t R = 0xff, W = 0xff; };
using RegSums = std::map<uint32_t, RegSum>;

struct Options
{
	std::set<uint32_t> setjmpThunks;                   // VAs of `jmp [setjmp slot]` stubs: direct calls to them are setjmp sites too
	uint32_t setjmpSlot = 0;                           // VA of the IAT slot of _setjmp3 (0 = none): calls through it get a longjmp landing pad
	const std::set<uint32_t>* funcEntries = nullptr;   // known function starts: jumps to them are tail calls
	const RegSums* regSums = nullptr;                  // register summaries (analyzeRegs): narrower saves around direct calls
};

// Register summaries of all functions in opt.funcEntries (fixpoint over the call graph).
RegSums analyzeRegs(Reader& r, const Options& opt);

// Lifts the function starting at `entry`. The generated C needs runtime/cpu.h.
Result liftFunction(Reader& r, uint32_t entry, const Options& opt);

// Opens/closes the shared Capstone handle (call once).
void init();
}
