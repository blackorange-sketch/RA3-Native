// OpenGL ES / EGL layer shared by the Android shell (console overlay) and the Direct3D 9 translation.
// One EGL context exists; the thread that owns it (initially the platform thread, later the game's render thread)
// is the only one allowed to issue GL calls. On non-Android builds everything is a stub.
#pragma once
#include <cstdint>

namespace gfx
{
#ifdef __ANDROID__
void setWindow(void* aNativeWindow);        // platform thread: window created (non-null) or destroyed (null)
bool ready();                               // a window surface exists
int width();
int height();
bool acquire();                             // make the calling thread the GL owner (waits for the platform thread to let go)
void servicePlatform();                     // platform thread: call regularly; releases the context when the game wants it
void postOverlay(const uint32_t* rgba, int w, int h);   // console picture (shown while the game draws nothing)
void drawOverlayAndPresent();               // platform thread, when it owns the context
bool ownedByGame();
// per-frame hooks for the D3D layer (game thread)
void beginFrame();
void swap();                  // present the window surface (game thread, after it drew the frame)
void drawOverlay();           // paint the console picture over the current frame
#else
inline void setWindow(void*) {}
inline bool ready() { return false; }
inline int width() { return 0; }
inline int height() { return 0; }
inline bool acquire() { return false; }
inline void servicePlatform() {}
inline void postOverlay(const uint32_t*, int, int) {}
inline void drawOverlayAndPresent() {}
inline bool ownedByGame() { return false; }
inline void beginFrame() {}
inline void swap() {}
inline void drawOverlay() {}
#endif
}
