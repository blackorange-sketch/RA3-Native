// see vksh.h
#include "vksh.h"
#include "rt.h"
#include "d3d9_sh.h"
#include "d3d9_tex.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <unordered_set>
#ifdef RA3_GLSLANG
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#endif

using rt::log;

namespace vksh
{
namespace
{
std::once_flag initOnce;
std::string cacheDir;
std::atomic<uint32_t> nCompiled{0}, nCached{0}, nFailed{0};
std::atomic<uint64_t> usCompile{0};

void init()
{
	std::call_once(initOnce, [] {
#ifdef RA3_GLSLANG
		glslang::InitializeProcess();
#endif
		cacheDir = rt::config.dataRoot + "/spvcache";
		mkdir(cacheDir.c_str(), 0755);
	});
}

uint64_t nowUs() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

// spirv-opt before the driver (built with SPIRV-Tools; /sdcard/ra3_noopt: off)
bool optimize()
{
#ifdef RA3_SPVOPT
	static const bool on = [] { bool off = access("/sdcard/ra3_noopt", F_OK) == 0; rt::log("vulkan: shaders optimised by spirv-opt: %s", off ? "no (ra3_noopt)" : "yes"); return !off; }();
	return on;
#else
	return false;
#endif
}

bool available()
{
#ifdef RA3_GLSLANG
	return true;
#else
	return false;
#endif
}

bool compile(const std::string& glsl, bool vertex, std::vector<uint32_t>& spv, std::string& err)
{
	init();
	spv.clear();
	char name[40];
	snprintf(name, sizeof name, "/%016llx%c%s.spv", (unsigned long long)d3d::tex::hashBytes((const uint8_t*)glsl.data(), glsl.size(), 0x5350560000000000ull), vertex ? 'v' : 'p', optimize() ? "o" : "");
	std::string path = cacheDir + name;
	std::vector<uint8_t> raw;
	if (d3d::tex::readWhole(path, raw) && raw.size() >= 20 && raw.size() % 4 == 0)
	{
		spv.resize(raw.size() / 4);
		memcpy(spv.data(), raw.data(), raw.size());
		if (spv[0] == 0x07230203u) { nCached++; return true; }
		spv.clear();
	}
#ifdef RA3_GLSLANG
	uint64_t t0 = nowUs();
	EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader sh(stage);
	const char* src = glsl.c_str();
	sh.setStrings(&src, 1);
	sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
	sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
	if (!sh.parse(GetDefaultResources(), 450, false, EShMsgDefault))
	{
		err = sh.getInfoLog();
		nFailed++;
		return false;
	}
	glslang::TProgram prog;
	prog.addShader(&sh);
	if (!prog.link(EShMsgDefault)) { err = prog.getInfoLog(); nFailed++; return false; }
	glslang::SpvOptions opt;
	opt.disableOptimizer = !optimize();   // spirv-opt performance passes (when built with SPIRV-Tools)
	glslang::GlslangToSpv(*prog.getIntermediate(stage), spv, &opt);
	usCompile += nowUs() - t0;
	nCompiled++;
	d3d::tex::writeWhole(path, std::vector<uint8_t>((const uint8_t*)spv.data(), (const uint8_t*)(spv.data() + spv.size())));
	return !spv.empty();
#else
	err = "glslang is not built in";
	return false;
#endif
}

// ---- stage 2 check: a background thread compiles every shader the game creates
namespace
{
std::mutex qmu;
std::condition_variable qcv;
std::deque<std::vector<uint8_t>> queue;
std::unordered_set<uint64_t> seen;
bool started = false;
std::atomic<uint32_t> nShaders{0}, nTransFail{0};

void logStats(uint32_t n)
{
	uint32_t c = nCompiled;
	log("vulkan: shaders %u: %u compiled (%.1f ms each), %u from the disk cache, %u failed, %u not translated", n, c,
		c ? usCompile.load() / 1000.0 / c : 0.0, nCached.load(), nFailed.load(), nTransFail.load());
}

void worker()
{
	uint32_t logged = 0;
	for (;;)
	{
		std::vector<uint8_t> bc;
		{
			std::unique_lock<std::mutex> l(qmu);
			// a summary once the queue has been empty for a second (loading done), not after every shader
			if (!qcv.wait_for(l, std::chrono::seconds(1), [] { return !queue.empty(); }))
			{
				uint32_t n = nShaders;
				if (n != logged) { logged = n; l.unlock(); logStats(n); }
				continue;
			}
			bc = std::move(queue.front());
			queue.pop_front();
		}
		d3d::ShaderInfo si;
		if (!d3d::shdr::translate(bc.data(), bc.size(), si)) { nTransFail++; nShaders++; continue; }
		std::string src = d3d::shdr::composeVk(si);   // a pixel shader on its own: every input declared
		std::vector<uint32_t> spv;
		std::string err;
		if (!compile(src, si.isVs, spv, err) && nFailed <= 5)
			log("vulkan: shader does not compile: %s\n%s", err.c_str(), src.c_str());
		nShaders++;
	}
}
}

void check(const std::vector<uint8_t>& byteCode)
{
	uint64_t h = d3d::tex::hashBytes(byteCode.data(), byteCode.size(), 0);
	std::lock_guard<std::mutex> l(qmu);
	if (!seen.insert(h).second) return;
	if (!started)
	{
		started = true;
		log("vulkan: checking the game's shaders in the background (%s)", available() ? "glslang built in" : "no glslang in this build");
		std::thread(worker).detach();
	}
	queue.push_back(byteCode);
	qcv.notify_one();
}
}
