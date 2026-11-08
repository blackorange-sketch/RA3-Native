// Custom on-screen keyboard buttons (side panels) with an in-game editor.
#pragma once
#include <functional>
#include <vector>

namespace hud
{
struct KeyEv { int vk; bool down; bool ctrl; };
using DrawRect = std::function<void(int x, int y, int w, int h, float r, float g, float b)>;

void setScreen(int sw, int sh);
// Touch handling; returns true when the event belongs to the HUD (it is then not passed to the game). Key events to inject go to out.
bool onTouch(int action, int idx, int count, const float* xs, const float* ys, std::vector<KeyEv>& out);
void draw(const DrawRect& rect);
}
