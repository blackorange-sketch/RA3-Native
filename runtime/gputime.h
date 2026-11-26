// GPU time per render pass, with /sdcard/ra3_prof (gputime.cpp). Call on the render thread.
#pragma once
#include <cstdint>
namespace gputime
{
bool on();                                                     // /sdcard/ra3_prof exists (any thread)
uint64_t key(int kind, uint32_t w, uint32_t h, uint32_t fmt);  // kind: 0 back buffer, 1 render target, 2 present
void mark(uint64_t key, uint32_t prevInfo, uint32_t id);       // a new render pass (target object id) starts; info of the one that ends:
                                                               // draws | clear flags << 24 (1 C, 2 Z, 4 S, 8 before any draw; 0x10.. after draws)
void frameEnd(uint32_t lastInfo);                             // before the swap: collect finished results, log every 900 frames
}
