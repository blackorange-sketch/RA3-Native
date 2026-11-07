// Input bridge: the Android layer pushes touch / key events here; the guest picks them up as Win32 window messages.
#pragma once
#include <cstdint>

namespace input
{
// Where the (letterboxed) back buffer sits on the screen; set on every present.
void setBackSize(int w, int h);   // the game's client area (D3D back buffer) changed
void backSize(int& w, int& h);
void setView(int vx, int vy, int vw, int vh, int backW, int backH);

// Touch event from Android. action: AMOTION_EVENT_ACTION_* (masked), idx = index of the pointer that changed.
void onTouch(int action, int idx, int count, const float* xs, const float* ys);
// A typed UTF-16 code unit from the soft keyboard.
void onChar(unsigned codeUnit);
// Android key event (AKEYCODE_*), down/up; unicode may be 0.
void onKey(bool down, int androidKey, int metaState);
// Screen size, and the on-screen keyboard button rectangle (screen pixels, top-left origin).
void setScreen(int sw, int sh);
void kbButtonRect(int& x, int& y, int& w, int& h);
void modButtonRect(int idx, int& x, int& y, int& w, int& h);   // 0 = Ctrl (force attack), 1 = Alt (waypoint)
bool modActive(int idx);
// True when the keyboard button was tapped (consumed by the call).
bool takeKeyboardToggle();
}
