// GL command recorder: the game threads never call the driver directly. Every GL call of the renderer is
// appended to a queue that a dedicated render thread (which owns the EGL context) replays in order, so the
// driver's CPU work and the frame swap overlap with the game's next frame.
//
//  * value-only calls are queued with their arguments evaluated at the call site;
//  * calls that pass memory (uniform arrays, buffer/texture data) copy that memory into the queue, so the
//    game may reuse its buffers right after the call (exactly as with a synchronous driver);
//  * calls that return something (glGen*, glGet*, link status...) run synchronously: the caller waits until
//    the render thread has executed everything queued before, then gets the result. They are rare
//    (resource creation, first use of a program).
//
// Create /sdcard/ra3_st to run single-threaded (every call goes straight to the driver, as before).
#pragma once
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace d3d
{
namespace rec
{
extern bool gOn;                         // threaded mode (decided once, before the first command)
extern pthread_t gRenderThread;           // the render thread (0 before it starts)
// pthread_self() is a plain TLS-register read on bionic; a thread_local bool went through emutls here
inline bool active() { return gOn && !pthread_equal(pthread_self(), gRenderThread); }

// recursive producer lock: the owner check is an inline relaxed load (std::recursive_mutex cost a pthread
// call + tid lookup per queued GL command). Other game threads do queue GL work too (the profile showed
// them colliding), so the inner lock is a real mutex that sleeps instead of spinning.
struct RecLock
{
	std::atomic<pthread_t> owner{0};
	std::mutex m;
	int depth = 0;
	void lock()
	{
		pthread_t me = pthread_self();
		if (owner.load(std::memory_order_relaxed) == me) { depth++; return; }
		m.lock();
		owner.store(me, std::memory_order_relaxed);
		depth = 1;
	}
	void unlock()
	{
		if (--depth) return;
		owner.store(0, std::memory_order_relaxed);
		m.unlock();
	}
};
	std::atomic<bool> held{false};
	int depth = 0;
	void lock()
	{
		pthread_t me = pthread_self();
		if (owner.load(std::memory_order_relaxed) == me) { depth++; return; }
		for (int i = 0; held.exchange(true, std::memory_order_acquire); i++)
			if (i > 64) sched_yield();
		owner.store(me, std::memory_order_relaxed);
		depth = 1;
	}
	void unlock()
	{
		if (--depth) return;
		owner.store(0, std::memory_order_relaxed);
		held.store(false, std::memory_order_release);
	}
};

struct Cmd { void (*run)(void*); uint32_t total; uint32_t pad; };

// reserve one command of `payload` bytes (+ extra); returns the payload address. Caller holds the producer lock.
void* reserve(size_t payload, void (*run)(void*));
extern RecLock gProducer;
inline RecLock& producerLock() { return gProducer; }
void publish();                          // hand the current chunk to the render thread
void waitIdle();                         // until everything queued so far has executed
void endFrame();                         // frame boundary: publish, and keep at most one frame in flight
void start(void (*threadInit)());       // start the render thread (runs threadInit on it first)
uint64_t renderBusyUs();                 // render thread time spent executing commands (resets)
int renderTid();                         // host tid of the render thread (0 before it starts)

template <class T> static void runAndDestroy(void* p) { T* t = (T*)p; (*t)(); t->~T(); }

template <class F> inline void q(F&& f)
{
	using T = typename std::decay<F>::type;
	std::lock_guard<RecLock> l(producerLock());
	void* p = reserve(sizeof(T), &runAndDestroy<T>);
	new (p) T(std::forward<F>(f));
}

// queue a call with a private copy of `n` bytes at `src`; make(copyPtr) builds the closure.
template <class Make> inline void qd(const void* src, size_t n, Make make)
{
	using T = decltype(make((const uint8_t*)nullptr));
	std::lock_guard<RecLock> l(producerLock());
	size_t tsz = (sizeof(T) + 15) & ~(size_t)15;
	void* p = reserve(tsz + n, &runAndDestroy<T>);
	uint8_t* data = (uint8_t*)p + tsz;
	if (n && src) memcpy(data, src, n);
	new (p) T(make(src ? (const uint8_t*)data : nullptr));
}

// run synchronously on the render thread and return the result
template <class F> inline auto s(F f) -> decltype(f())
{
	using R = decltype(f());
	if (!active()) return f();
	struct Box { std::mutex m; std::condition_variable cv; bool done = false; };
	Box box;
	typename std::conditional<std::is_void<R>::value, int, R>::type* out = nullptr;
	if constexpr (std::is_void<R>::value)
	{
		q([&box, &f] { f(); std::lock_guard<std::mutex> l(box.m); box.done = true; box.cv.notify_all(); });
		publish();
		std::unique_lock<std::mutex> l(box.m);
		box.cv.wait(l, [&] { return box.done; });
		(void)out;
	}
	else
	{
		R r{};
		q([&box, &f, &r] { r = f(); std::lock_guard<std::mutex> l(box.m); box.done = true; box.cv.notify_all(); });
		publish();
		std::unique_lock<std::mutex> l(box.m);
		box.cv.wait(l, [&] { return box.done; });
		return r;
	}
}

// value-only call
template <class... P, class... A> inline void call(void (*f)(P...), A... a)
{
	if (!active()) { f(a...); return; }
	q([f, t = std::tuple<P...>((P)a...)] { std::apply(f, t); });
}
// synchronous call (results or pointer arguments that must stay valid)
template <class R, class... P, class... A> inline R scall(R (*f)(P...), A... a)
{
	if (!active()) return f(a...);
	return s([&] { return f(a...); });
}

extern GLint gUnpackAlign;
size_t texBytes(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei d);

// ---- calls carrying memory
inline void Uniform4fv(GLint l, GLsizei c, const GLfloat* v)
{
	if (!active()) { ::glUniform4fv(l, c, v); return; }
	qd(v, (size_t)c * 16, [l, c](const uint8_t* p) { return [l, c, p] { ::glUniform4fv(l, c, (const GLfloat*)p); }; });
}
inline void Uniform4iv(GLint l, GLsizei c, const GLint* v)
{
	if (!active()) { ::glUniform4iv(l, c, v); return; }
	qd(v, (size_t)c * 16, [l, c](const uint8_t* p) { return [l, c, p] { ::glUniform4iv(l, c, (const GLint*)p); }; });
}
inline void Uniform1iv(GLint l, GLsizei c, const GLint* v)
{
	if (!active()) { ::glUniform1iv(l, c, v); return; }
	qd(v, (size_t)c * 4, [l, c](const uint8_t* p) { return [l, c, p] { ::glUniform1iv(l, c, (const GLint*)p); }; });
}
inline void SamplerParameterfv(GLuint s, GLenum n, const GLfloat* v)
{
	if (!active()) { ::glSamplerParameterfv(s, n, v); return; }
	qd(v, 16, [s, n](const uint8_t* p) { return [s, n, p] { ::glSamplerParameterfv(s, n, (const GLfloat*)p); }; });
}
inline void BufferData(GLenum t, GLsizeiptr n, const void* d, GLenum u)
{
	if (!active()) { ::glBufferData(t, n, d, u); return; }
	qd(d, d ? (size_t)n : 0, [t, n, u](const uint8_t* p) { return [t, n, u, p] { ::glBufferData(t, n, p, u); }; });
}
inline void BufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void* d)
{
	if (!active()) { ::glBufferSubData(t, o, n, d); return; }
	qd(d, (size_t)n, [t, o, n](const uint8_t* p) { return [t, o, n, p] { ::glBufferSubData(t, o, n, p); }; });
}
inline void TexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void* d)
{
	if (!active()) { ::glTexImage2D(t, l, i, w, h, b, f, ty, d); return; }
	qd(d, d ? texBytes(f, ty, w, h, 1) : 0, [=](const uint8_t* p) { return [=] { ::glTexImage2D(t, l, i, w, h, b, f, ty, p); }; });
}
inline void TexImage3D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLsizei dd, GLint b, GLenum f, GLenum ty, const void* d)
{
	if (!active()) { ::glTexImage3D(t, l, i, w, h, dd, b, f, ty, d); return; }
	qd(d, d ? texBytes(f, ty, w, h, dd) : 0, [=](const uint8_t* p) { return [=] { ::glTexImage3D(t, l, i, w, h, dd, b, f, ty, p); }; });
}
inline void CompressedTexImage2D(GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei n, const void* d)
{
	if (!active()) { ::glCompressedTexImage2D(t, l, i, w, h, b, n, d); return; }
	qd(d, (size_t)n, [=](const uint8_t* p) { return [=] { ::glCompressedTexImage2D(t, l, i, w, h, b, n, p); }; });
}
inline void DeleteBuffers(GLsizei n, const GLuint* v)
{
	if (!active()) { ::glDeleteBuffers(n, v); return; }
	qd(v, (size_t)n * 4, [n](const uint8_t* p) { return [n, p] { ::glDeleteBuffers(n, (const GLuint*)p); }; });
}
inline void DeleteTextures(GLsizei n, const GLuint* v)
{
	if (!active()) { ::glDeleteTextures(n, v); return; }
	qd(v, (size_t)n * 4, [n](const uint8_t* p) { return [n, p] { ::glDeleteTextures(n, (const GLuint*)p); }; });
}
inline void PixelStorei(GLenum n, GLint v)
{
	if (n == GL_UNPACK_ALIGNMENT) gUnpackAlign = v;
	call(&::glPixelStorei, n, v);
}
}   // namespace rec
}   // namespace d3d

// ---- route the renderer's GL calls through the recorder
#define glActiveTexture(...) ::d3d::rec::call(&::glActiveTexture, __VA_ARGS__)
#define glAttachShader(...) ::d3d::rec::call(&::glAttachShader, __VA_ARGS__)
#define glBindBuffer(...) ::d3d::rec::call(&::glBindBuffer, __VA_ARGS__)
#define glBindFramebuffer(...) ::d3d::rec::call(&::glBindFramebuffer, __VA_ARGS__)
#define glBindRenderbuffer(...) ::d3d::rec::call(&::glBindRenderbuffer, __VA_ARGS__)
#define glBindSampler(...) ::d3d::rec::call(&::glBindSampler, __VA_ARGS__)
#define glBindTexture(...) ::d3d::rec::call(&::glBindTexture, __VA_ARGS__)
#define glGenerateMipmap(...) ::d3d::rec::call(&::glGenerateMipmap, __VA_ARGS__)
#define glBindVertexArray(...) ::d3d::rec::call(&::glBindVertexArray, __VA_ARGS__)
#define glBlendColor(...) ::d3d::rec::call(&::glBlendColor, __VA_ARGS__)
#define glBlendEquation(...) ::d3d::rec::call(&::glBlendEquation, __VA_ARGS__)
#define glBlendEquationSeparate(...) ::d3d::rec::call(&::glBlendEquationSeparate, __VA_ARGS__)
#define glBlendFunc(...) ::d3d::rec::call(&::glBlendFunc, __VA_ARGS__)
#define glBlendFuncSeparate(...) ::d3d::rec::call(&::glBlendFuncSeparate, __VA_ARGS__)
#define glBlitFramebuffer(...) ::d3d::rec::call(&::glBlitFramebuffer, __VA_ARGS__)
#define glClear(...) ::d3d::rec::call(&::glClear, __VA_ARGS__)
#define glClearColor(...) ::d3d::rec::call(&::glClearColor, __VA_ARGS__)
#define glClearDepthf(...) ::d3d::rec::call(&::glClearDepthf, __VA_ARGS__)
#define glClearStencil(...) ::d3d::rec::call(&::glClearStencil, __VA_ARGS__)
#define glColorMask(...) ::d3d::rec::call(&::glColorMask, __VA_ARGS__)
#define glCompileShader(...) ::d3d::rec::call(&::glCompileShader, __VA_ARGS__)
#define glCullFace(...) ::d3d::rec::call(&::glCullFace, __VA_ARGS__)
#define glDeleteShader(...) ::d3d::rec::call(&::glDeleteShader, __VA_ARGS__)
#define glDepthFunc(...) ::d3d::rec::call(&::glDepthFunc, __VA_ARGS__)
#define glDepthMask(...) ::d3d::rec::call(&::glDepthMask, __VA_ARGS__)
#define glDepthRangef(...) ::d3d::rec::call(&::glDepthRangef, __VA_ARGS__)
#define glDisable(...) ::d3d::rec::call(&::glDisable, __VA_ARGS__)
#define glDisableVertexAttribArray(...) ::d3d::rec::call(&::glDisableVertexAttribArray, __VA_ARGS__)
#define glDrawArrays(...) ::d3d::rec::call(&::glDrawArrays, __VA_ARGS__)
#define glDrawArraysInstanced(...) ::d3d::rec::call(&::glDrawArraysInstanced, __VA_ARGS__)
#define glDrawElements(...) ::d3d::rec::call(&::glDrawElements, __VA_ARGS__)
#define glDrawElementsInstanced(...) ::d3d::rec::call(&::glDrawElementsInstanced, __VA_ARGS__)
#define glEnable(...) ::d3d::rec::call(&::glEnable, __VA_ARGS__)
#define glEnableVertexAttribArray(...) ::d3d::rec::call(&::glEnableVertexAttribArray, __VA_ARGS__)
#define glFramebufferRenderbuffer(...) ::d3d::rec::call(&::glFramebufferRenderbuffer, __VA_ARGS__)
#define glFramebufferTexture2D(...) ::d3d::rec::call(&::glFramebufferTexture2D, __VA_ARGS__)
#define glFrontFace(...) ::d3d::rec::call(&::glFrontFace, __VA_ARGS__)
#define glLinkProgram(...) ::d3d::rec::call(&::glLinkProgram, __VA_ARGS__)
#define glPolygonOffset(...) ::d3d::rec::call(&::glPolygonOffset, __VA_ARGS__)
#define glRenderbufferStorage(...) ::d3d::rec::call(&::glRenderbufferStorage, __VA_ARGS__)
#define glSamplerParameterf(...) ::d3d::rec::call(&::glSamplerParameterf, __VA_ARGS__)
#define glGetFloatv(...) ::d3d::rec::scall(&::glGetFloatv, __VA_ARGS__)
#define glSamplerParameteri(...) ::d3d::rec::call(&::glSamplerParameteri, __VA_ARGS__)
#define glScissor(...) ::d3d::rec::call(&::glScissor, __VA_ARGS__)
#define glStencilFunc(...) ::d3d::rec::call(&::glStencilFunc, __VA_ARGS__)
#define glStencilMask(...) ::d3d::rec::call(&::glStencilMask, __VA_ARGS__)
#define glStencilOp(...) ::d3d::rec::call(&::glStencilOp, __VA_ARGS__)
#define glTexParameteri(...) ::d3d::rec::call(&::glTexParameteri, __VA_ARGS__)
#define glUniform1f(...) ::d3d::rec::call(&::glUniform1f, __VA_ARGS__)
#define glUniform1i(...) ::d3d::rec::call(&::glUniform1i, __VA_ARGS__)
#define glUniform2f(...) ::d3d::rec::call(&::glUniform2f, __VA_ARGS__)
#define glUniform4f(...) ::d3d::rec::call(&::glUniform4f, __VA_ARGS__)
#define glUseProgram(...) ::d3d::rec::call(&::glUseProgram, __VA_ARGS__)
#define glVertexAttrib4f(...) ::d3d::rec::call(&::glVertexAttrib4f, __VA_ARGS__)
#define glVertexAttribDivisor(...) ::d3d::rec::call(&::glVertexAttribDivisor, __VA_ARGS__)
#define glVertexAttribPointer(...) ::d3d::rec::call(&::glVertexAttribPointer, __VA_ARGS__)
#define glViewport(...) ::d3d::rec::call(&::glViewport, __VA_ARGS__)
#define glDeleteSync(...) ::d3d::rec::call(&::glDeleteSync, __VA_ARGS__)
#define glDeleteBuffers(...) ::d3d::rec::DeleteBuffers(__VA_ARGS__)
#define glDeleteTextures(...) ::d3d::rec::DeleteTextures(__VA_ARGS__)
// memory-carrying
#define glUniform4fv(...) ::d3d::rec::Uniform4fv(__VA_ARGS__)
#define glUniform4iv(...) ::d3d::rec::Uniform4iv(__VA_ARGS__)
#define glUniform1iv(...) ::d3d::rec::Uniform1iv(__VA_ARGS__)
#define glSamplerParameterfv(...) ::d3d::rec::SamplerParameterfv(__VA_ARGS__)
#define glBufferData(...) ::d3d::rec::BufferData(__VA_ARGS__)
#define glBufferSubData(...) ::d3d::rec::BufferSubData(__VA_ARGS__)
#define glTexImage2D(...) ::d3d::rec::TexImage2D(__VA_ARGS__)
#define glTexImage3D(...) ::d3d::rec::TexImage3D(__VA_ARGS__)
#define glCompressedTexImage2D(...) ::d3d::rec::CompressedTexImage2D(__VA_ARGS__)
#define glPixelStorei(...) ::d3d::rec::PixelStorei(__VA_ARGS__)
// synchronous (results / out-pointers / strings)
#define glGenBuffers(...) ::d3d::rec::scall(&::glGenBuffers, __VA_ARGS__)
#define glGenTextures(...) ::d3d::rec::scall(&::glGenTextures, __VA_ARGS__)
#define glGenFramebuffers(...) ::d3d::rec::scall(&::glGenFramebuffers, __VA_ARGS__)
#define glGenRenderbuffers(...) ::d3d::rec::scall(&::glGenRenderbuffers, __VA_ARGS__)
#define glGenSamplers(...) ::d3d::rec::scall(&::glGenSamplers, __VA_ARGS__)
#define glGenVertexArrays(...) ::d3d::rec::scall(&::glGenVertexArrays, __VA_ARGS__)
#define glCreateShader(...) ::d3d::rec::scall(&::glCreateShader, __VA_ARGS__)
#define glCreateProgram(...) ::d3d::rec::scall(&::glCreateProgram)
#define glShaderSource(...) ::d3d::rec::scall(&::glShaderSource, __VA_ARGS__)
#define glBindAttribLocation(...) ::d3d::rec::scall(&::glBindAttribLocation, __VA_ARGS__)
#define glGetUniformLocation(...) ::d3d::rec::scall(&::glGetUniformLocation, __VA_ARGS__)
#define glGetString(...) ::d3d::rec::scall(&::glGetString, __VA_ARGS__)
#define glGetIntegerv(...) ::d3d::rec::scall(&::glGetIntegerv, __VA_ARGS__)
#define glGetError(...) ::d3d::rec::scall(&::glGetError)
#define glCheckFramebufferStatus(...) ::d3d::rec::scall(&::glCheckFramebufferStatus, __VA_ARGS__)
#define glGetShaderiv(...) ::d3d::rec::scall(&::glGetShaderiv, __VA_ARGS__)
#define glGetShaderInfoLog(...) ::d3d::rec::scall(&::glGetShaderInfoLog, __VA_ARGS__)
#define glGetProgramiv(...) ::d3d::rec::scall(&::glGetProgramiv, __VA_ARGS__)
#define glGetProgramInfoLog(...) ::d3d::rec::scall(&::glGetProgramInfoLog, __VA_ARGS__)
#define glIsShader(...) ::d3d::rec::scall(&::glIsShader, __VA_ARGS__)
#define glReadPixels(...) ::d3d::rec::scall(&::glReadPixels, __VA_ARGS__)
#define glProgramParameteri(...) ::d3d::rec::call(&::glProgramParameteri, __VA_ARGS__)
#define glProgramBinary(...) ::d3d::rec::scall(&::glProgramBinary, __VA_ARGS__)
#define glGetProgramBinary(...) ::d3d::rec::scall(&::glGetProgramBinary, __VA_ARGS__)
#define glFinish(...) ::d3d::rec::scall(&::glFinish)
#define glMapBufferRange(...) ::d3d::rec::scall(&::glMapBufferRange, __VA_ARGS__)
#define glUnmapBuffer(...) ::d3d::rec::scall(&::glUnmapBuffer, __VA_ARGS__)
#define glFenceSync(...) ::d3d::rec::scall(&::glFenceSync, __VA_ARGS__)
#define glClientWaitSync(...) ::d3d::rec::scall(&::glClientWaitSync, __VA_ARGS__)
