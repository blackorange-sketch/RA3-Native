// GPU time per render pass, with /sdcard/ra3_prof (gputime.cpp). Call on the render thread.
#pragma once
#include <cstdint>
namespace gputime
{
bool on();                                                     // /sdcard/ra3_prof exists (any thread)
uint64_t key(int kind, uint32_t w, uint32_t h, uint32_t fmt);  // kind: 0 back buffer, 1 render target, 2 present
void mark(uint64_t key);                                       // a new render pass starts
void frameEnd();                                               // before the swap: collect finished results, log every 900 frames
}
