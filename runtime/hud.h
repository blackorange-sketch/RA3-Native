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
// Screen width taken by the button columns on the left and right (wide screen mode keeps the game picture out of them).
void reserves(int sw, int sh, int& left, int& right);
bool editingNow();
int buttonUnit();   // the on-screen buttons' size unit (launcher: button size)   // the button editor is open
// Wide screen: the keyboard button's place in the top row (false: the default top-left corner).
bool kbRect(int& x, int& y, int& w, int& h);
}
