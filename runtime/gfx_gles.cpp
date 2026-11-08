// EGL/GLES3 backend (Android only).
#ifdef __ANDROID__
#include "gfx.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <unistd.h>
#include <sys/syscall.h>
#ifndef gettid
#define gettid() ((int)syscall(SYS_gettid))
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "rt.h"

namespace gfx
{
namespace
{
std::mutex mu;                       // protects the fields below (not GL itself)
std::condition_variable cv;
EGLDisplay dpy = EGL_NO_DISPLAY;
EGLConfig cfg = nullptr;
EGLContext ctx = EGL_NO_CONTEXT;
EGLSurface surf = EGL_NO_SURFACE;
ANativeWindow* win = nullptr;
int surfW = 0, surfH = 0;
std::atomic<uint32_t> surfGen{0};    // bumped whenever the surface changes
std::thread::id owner;               // thread the context is current on
bool ownerIsGame = false;
bool wantGame = false;
bool curValid = false;
uint32_t curGen = 0;

std::vector<uint32_t> overlay;       // posted console picture
int ovW = 0, ovH = 0;
bool ovDirty = false;

GLuint ovProg = 0, ovTex = 0;

void egl(const char* what)
{
	EGLint e = eglGetError();
	if (e != EGL_SUCCESS) rt::log("EGL: %s failed (0x%x)", what, e);
}

bool makeCurrentHere()
{
	if (surf == EGL_NO_SURFACE) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); return false; }
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) { egl("eglMakeCurrent"); return false; }
	curGen = surfGen;
	eglQuerySurface(dpy, surf, EGL_WIDTH, &surfW);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &surfH);
	return true;
}

GLuint compile(GLenum type, const char* src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char buf[512]; glGetShaderInfoLog(s, sizeof buf, nullptr, buf); rt::log("GL shader error: %s", buf); }
	return s;
}

void initOverlayGL()
{
	if (ovProg) return;
	const char* vs = "#version 300 es\nout vec2 uv;\nvoid main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = vec2(p.x, 1.0 - p.y); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
	const char* fs = "#version 300 es\nprecision mediump float;\nin vec2 uv;\nuniform sampler2D t;\nout vec4 o;\nvoid main(){ o = texture(t, uv); }\n";
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
	ovProg = glCreateProgram();
	glAttachShader(ovProg, v);
	glAttachShader(ovProg, f);
	glLinkProgram(ovProg);
	glGenTextures(1, &ovTex);
	glBindTexture(GL_TEXTURE_2D, ovTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void drawOverlayImpl()   // caller owns the context
{
	std::lock_guard<std::mutex> l(mu);
	if (overlay.empty()) return;
	initOverlayGL();
	glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
	glColorMask(1, 1, 1, 1);
	glViewport(0, 0, surfW, surfH);
	glUseProgram(ovProg);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ovTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (ovDirty) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ovW, ovH, 0, GL_RGBA, GL_UNSIGNED_BYTE, overlay.data()); ovDirty = false; }
	glUniform1i(glGetUniformLocation(ovProg, "t"), 0);
	glBindVertexArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}
}   // namespace

void setWindow(void* w)
{
	std::lock_guard<std::mutex> l(mu);
	if (!w)
	{
		if (surf != EGL_NO_SURFACE)
		{
			if (!ownerIsGame) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); curValid = false; }
			eglDestroySurface(dpy, surf);
			surf = EGL_NO_SURFACE;
			surfGen++;
		}
		win = nullptr;
		return;
	}
	win = (ANativeWindow*)w;
	if (dpy == EGL_NO_DISPLAY)
	{
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglInitialize(dpy, nullptr, nullptr);
		const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
		EGLint n = 0;
		if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1)
		{
			const EGLint cb[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
			eglChooseConfig(dpy, cb, &cfg, 1, &n);
		}
		const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
		egl("eglCreateContext");
	}
	surf = eglCreateWindowSurface(dpy, cfg, win, nullptr);
	egl("eglCreateWindowSurface");
	surfGen++;
	if (surf == EGL_NO_SURFACE) return;
	if (ownerIsGame)
	{
		eglQuerySurface(dpy, surf, EGL_WIDTH, &surfW);
		eglQuerySurface(dpy, surf, EGL_HEIGHT, &surfH);
		rt::log("EGL: new surface %dx%d (the game thread picks it up at the next present)", surfW, surfH);
		return;
	}
	owner = std::this_thread::get_id();
	curValid = makeCurrentHere();
	#if __has_include("build_id.h")
	#include "build_id.h"
	rt::log("build %.8s", RA3_BUILD_ID);
	#endif
	rt::log("EGL: surface %dx%d, GL %s / %s", surfW, surfH, (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
}

bool ready() { std::lock_guard<std::mutex> l(mu); return surf != EGL_NO_SURFACE; }
int width() { return surfW; }
int height() { return surfH; }
bool ownedByGame() { return ownerIsGame; }
static std::atomic<int> ownerTid{0};
bool isOwner() { static thread_local int t = (int)gettid(); return ownerIsGame && ownerTid.load() == t; }
void release()
{
	std::unique_lock<std::mutex> l(mu);
	if (!ownerIsGame || owner != std::this_thread::get_id()) return;
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	curValid = false;
	owner = std::thread::id();
	ownerTid = 0;
	cv.notify_all();
}

bool acquire()
{
	std::unique_lock<std::mutex> l(mu);
	if (surf == EGL_NO_SURFACE) return false;
	if (ownerIsGame && owner == std::this_thread::get_id()) return true;
	wantGame = true;
	// the platform thread releases the context in servicePlatform()
	if (!cv.wait_for(l, std::chrono::seconds(3), [] { return !curValid || owner == std::thread::id(); }))
		rt::log("EGL: platform thread did not release the context");
	wantGame = false;
	owner = std::this_thread::get_id();
	ownerIsGame = true;
	ownerTid = (int)gettid();
	bool ok = makeCurrentHere();
	curValid = ok;
	{ static int n = 0; if (n++ < 6) rt::log("EGL: game thread owns the context (%s) tid %d", ok ? "ok" : "FAILED", (int)gettid()); }
	return ok;
}

void servicePlatform()
{
	std::unique_lock<std::mutex> l(mu);
	if (wantGame && !ownerIsGame && owner == std::this_thread::get_id())
	{
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		curValid = false;
		owner = std::thread::id();
		cv.notify_all();
	}
}

void postOverlay(const uint32_t* rgba, int w, int h)
{
	std::lock_guard<std::mutex> l(mu);
	overlay.assign(rgba, rgba + (size_t)w * h);
	ovW = w; ovH = h; ovDirty = true;
}

void drawOverlayAndPresent()
{
	{
		std::lock_guard<std::mutex> l(mu);
		if (surf == EGL_NO_SURFACE || ownerIsGame || owner != std::this_thread::get_id()) return;
	}
	glClearColor(0.06f, 0.08f, 0.1f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	drawOverlayImpl();
	eglSwapBuffers(dpy, surf);
}

// ---- game thread
void beginFrame()
{
	if (surfGen != curGen)
	{
		std::lock_guard<std::mutex> l(mu);
		if (surf != EGL_NO_SURFACE) { makeCurrentHere(); curValid = true; }
	}
}

void swap()
{
	if (!ownerIsGame || surf == EGL_NO_SURFACE) return;
	eglSwapBuffers(dpy, surf);
}

void drawOverlay()
{
	if (!ownerIsGame || surf == EGL_NO_SURFACE) return;
	drawOverlayImpl();
}
}   // namespace gfx
#endif
