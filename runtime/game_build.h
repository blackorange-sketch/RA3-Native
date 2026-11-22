#pragma once
// The game build this library was recompiled from. The APK carries two libraries:
//   libra3native.so     - ra3_1.12.game of the retail / EA app release
//   libra3native113.so  - ra3_1.13.game of the Steam release (built with RA3_GAME_113)
// The launcher picks one by the game file it finds. Addresses below are the places the runtime touches directly;
// the 1.13 ones were translated from 1.12 by tools/match_builds.py (matching disassembly). 0: unknown for this build
// (the feature that needs it stays off).
#include <cstdint>

namespace gb {
#ifdef RA3_GAME_113
constexpr const char* kExe = "ra3_1.13.game";
constexpr const char* kVer = "1.13";
constexpr uint32_t ResModeCheck = 0xa93130;     // graphics options: display mode check for the resolution list (gate)
constexpr uint32_t GfxOptionsList = 0xa93a00;   // graphics options: LABELS / VALUES lists (debug hook)
constexpr uint32_t GfxOptionsList2 = 0xa92ed0;
constexpr uint32_t SetTextureWrapper = 0x4f64f0;
constexpr uint32_t ApplyStaticLOD = 0x646a20;   // GameLODManager::applyStaticLOD(level)
constexpr uint32_t LODManager = 0xcec448;       // global GameLODManager*
constexpr uint32_t FpsCap = 0xcb8514, LogicHz = 0xcb8510;
constexpr uint32_t LimiterRet = 0x652d13;       // return address of the frame limiter's Sleep (60 fps experiment)
constexpr uint32_t ThreadWrap1 = 0x9c4930, ThreadWrap2 = 0x9c4ba0;
constexpr uint32_t OptionsMgr = 0xce4c14, OptionsStore = 0xcf5204;
constexpr uint32_t FnStrInit = 0x4ccc30, FnStrFree = 0x4cbee0, FnOptHas = 0x9440a0, FnOptGet = 0x6450c0, FnOptCount = 0x645150;
constexpr bool kLodDiag = false;                // the LOD diagnostics read more globals that were not translated
#else
constexpr const char* kExe = "ra3_1.12.game";
constexpr const char* kVer = "1.12";
constexpr uint32_t ResModeCheck = 0xaf99f0;
constexpr uint32_t GfxOptionsList = 0xafa2a0;
constexpr uint32_t GfxOptionsList2 = 0xaf9790;
constexpr uint32_t SetTextureWrapper = 0x9c6870;
constexpr uint32_t ApplyStaticLOD = 0x61d660;
constexpr uint32_t LODManager = 0xce2f98;
constexpr uint32_t FpsCap = 0xcaf9d4, LogicHz = 0xcaf9d0;
constexpr uint32_t LimiterRet = 0x629863;
constexpr uint32_t ThreadWrap1 = 0xa2b5e0, ThreadWrap2 = 0xa2b850;
constexpr uint32_t OptionsMgr = 0xcdb754, OptionsStore = 0xcebd54;
constexpr uint32_t FnStrInit = 0x4cc910, FnStrFree = 0x4cbbc0, FnOptHas = 0x91b040, FnOptGet = 0x61c040, FnOptCount = 0x61c0d0;
constexpr bool kLodDiag = true;
#endif
}
