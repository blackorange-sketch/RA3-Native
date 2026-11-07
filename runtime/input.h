// Input bridge: the Android layer pushes touch / key events here; the guest picks them up as Win32 window messages.
#pragma once
#include <cstdint>

namespace input
{
// Where the (letterboxed) back buffer sits on the screen; set on every present.
void setView(int vx, int vy, int vw, int vh, int backW, int backH);

// Touch event from Android. action: AMOTION_EVENT_ACTION_* (masked), idx = index of the pointer that changed.
void onTouch(int action, int idx, int count, const float* xs, const float* ys);
// Android key event (AKEYCODE_*), down/up; unicode may be 0.
void onKey(bool down, int androidKey, int metaState);
// True when the 3-finger gesture asked to toggle the soft keyboard (consumed by the call).
bool takeKeyboardToggle();
}
