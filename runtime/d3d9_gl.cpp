#include <set>
#include <string>
#include <map>
#include <tuple>
#include <cstdio>
#include "input.h"
#include "hud.h"
// OpenGL ES 3 back end of the Direct3D 9 layer: resource upload, programs built from translated D3D9 shaders,
// render state mapping, draw calls, render targets and presentation. Android only; elsewhere everything is a no-op.
#include "d3d9.h"

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <sys/syscall.h>
#ifndef gettid
#define gettid() ((int)syscall(SYS_gettid))
#endif

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "gfx.h"

using namespace rt;

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace d3d
{
namespace glr
{
namespace
{
bool gInit = false, gOk = false;
GLuint gVao = 0, gBackFbo = 0, gBackTex = 0, gBackDepth = 0, gDrawFbo = 0, gReadFbo = 0, gUpVbo = 0, gUpIbo = 0;
GLuint gBlitProg = 0;
GLuint gSamplers[16] = {};
GLuint gDummy2D = 0, gDummyCube = 0, gDummy3D = 0;
bool gHasS3tc = false;
std::atomic<uint32_t> gDrawsThisFrame{0};
uint32_t gFrameNo = 0;
int gErrLogs = 0;

void glErr(const char* where)
{
	GLenum e;
	while ((e = glGetError()) != GL_NO_ERROR)
		if (gErrLogs++ < 40) log("GL error 0x%x at %s", e, where);
}

GLuint compileStage(GLenum type, const std::string& src, const char* what)
{
	GLuint s = glCreateShader(type);
	const char* p = src.c_str();
	glShaderSource(s, 1, &p, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char buf[1024] = {0};
		glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
		log("GL: %s shader compile failed (ctx %p, tid %d, glErr 0x%x, isShader %d): %s", what, (void*)eglGetCurrentContext(), (int)gettid(), glGetError(), (int)glIsShader(s), buf);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

// ---------------------------------------------------------------------------------------------------------------
// texture formats
struct TexFmt
{
	GLenum internal = GL_RGBA8, format = GL_RGBA, type = GL_UNSIGNED_BYTE;
	bool compressed = false, convert = false, floatNoFilter = false, depth = false, none = false;
	GLint swz[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
	bool swizzle = false;
};
TexFmt texFmtOf(uint32_t f)
{
	TexFmt t;
	switch (f)
	{
	case 20: case 21: case 22: case 24: case 25: case 26: t.convert = true; break;   // expanded to RGBA8 on the CPU
	case 23: t.internal = GL_RGB565; t.format = GL_RGB; t.type = GL_UNSIGNED_SHORT_5_6_5; break;
	case 28: t.internal = GL_R8; t.format = GL_RED; t.swizzle = true; t.swz[0] = GL_ZERO; t.swz[1] = GL_ZERO; t.swz[2] = GL_ZERO; t.swz[3] = GL_RED; break;
	case 50: t.internal = GL_R8; t.format = GL_RED; t.swizzle = true; t.swz[0] = GL_RED; t.swz[1] = GL_RED; t.swz[2] = GL_RED; t.swz[3] = GL_ONE; break;
	case 51: t.internal = GL_RG8; t.format = GL_RG; t.swizzle = true; t.swz[0] = GL_RED; t.swz[1] = GL_RED; t.swz[2] = GL_RED; t.swz[3] = GL_GREEN; break;
	case 111: t.internal = GL_R16F; t.format = GL_RED; t.type = GL_HALF_FLOAT; break;
	case 112: t.internal = GL_RG16F; t.format = GL_RG; t.type = GL_HALF_FLOAT; break;
	case 113: t.internal = GL_RGBA16F; t.format = GL_RGBA; t.type = GL_HALF_FLOAT; break;
	case 114: t.internal = GL_R32F; t.format = GL_RED; t.type = GL_FLOAT; t.floatNoFilter = true; break;
	case 115: t.internal = GL_RG32F; t.format = GL_RG; t.type = GL_FLOAT; t.floatNoFilter = true; break;
	case 116: t.internal = GL_RGBA32F; t.format = GL_RGBA; t.type = GL_FLOAT; t.floatNoFilter = true; break;
	case 0x31545844: t.compressed = true; t.internal = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
	case 0x33545844: t.compressed = true; t.internal = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
	case 0x35545844: t.compressed = true; t.internal = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
	case 80: t.depth = true; t.internal = GL_DEPTH_COMPONENT16; t.format = GL_DEPTH_COMPONENT; t.type = GL_UNSIGNED_SHORT; break;
	case 71: t.depth = true; t.internal = GL_DEPTH_COMPONENT32F; t.format = GL_DEPTH_COMPONENT; t.type = GL_FLOAT; break;
	case 75: case 77: case 0x5a544e49: case 0x34324644: case 0x36314644:
		t.depth = true; t.internal = GL_DEPTH24_STENCIL8; t.format = GL_DEPTH_STENCIL; t.type = GL_UNSIGNED_INT_24_8; break;
	case 0x4c4c554e: t.none = true; break;   // "NULL" render target
	default:
		if (f != 21 && f != 22) { static std::atomic<int> n{0}; if (n++ < 20) log("GL: unsupported texture format %u, using RGBA8", f); }
		t.convert = true;
		break;
	}
	return t;
}

// expands a level of a "convert" format to RGBA8
void expandLevel(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	out.resize((size_t)w * h * 4);
	uint32_t pitch = w * bpp(fmt);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
		{
			const uint8_t* s = src + (size_t)y * pitch + (size_t)x * bpp(fmt);
			uint8_t* d = &out[((size_t)y * w + x) * 4];
			switch (fmt)
			{
			case 21: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
			case 22: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; break;
			case 20: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; break;
			case 24: case 25:
			{
				uint16_t v = s[0] | (s[1] << 8);
				d[0] = ((v >> 10) & 31) * 255 / 31; d[1] = ((v >> 5) & 31) * 255 / 31; d[2] = (v & 31) * 255 / 31;
				d[3] = fmt == 25 ? ((v >> 15) ? 255 : 0) : 255;
				break;
			}
			case 26:
			{
				uint16_t v = s[0] | (s[1] << 8);
				d[0] = ((v >> 8) & 15) * 17; d[1] = ((v >> 4) & 15) * 17; d[2] = (v & 15) * 17; d[3] = ((v >> 12) & 15) * 17;
				break;
			}
			default: d[0] = d[1] = d[2] = 0; d[3] = 255; break;
			}
		}
}

GLenum targetOf(const Obj* o) { return o->iface == ICube ? GL_TEXTURE_CUBE_MAP : o->iface == IVolume ? GL_TEXTURE_3D : GL_TEXTURE_2D; }

void uploadTexture(Obj* o)
{
	TexFmt tf = texFmtOf(o->fmt);
	GLenum target = targetOf(o);
	glBindTexture(target, o->glTex);
	if (tf.compressed && !gHasS3tc) { tf.compressed = false; tf.convert = true; }   // TODO: software DXT decode
	if (!(o->rtTex && !o->data))
	{
		std::vector<uint8_t> tmp;
		for (uint32_t f = 0; f < o->faces; f++)
			for (uint32_t l = 0; l < o->levels; l++)
			{
				uint32_t w = std::max(1u, o->w >> l), h = std::max(1u, o->h >> l), d = std::max(1u, o->d >> l);
				const uint8_t* src = (const uint8_t*)guest::host(o->data + o->levelOff[f * o->levels + l]);
				GLenum tgt = o->iface == ICube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : target;
				if (tf.compressed)
				{
					uint32_t sz = levelBytes(o->fmt, w, h);
					glCompressedTexImage2D(tgt, l, tf.internal, w, h, 0, sz, src);
				}
				else if (tf.convert)
				{
					if (o->iface == IVolume)
					{
						std::vector<uint8_t> all;
						size_t slice = (size_t)w * h * bpp(o->fmt);
						for (uint32_t z = 0; z < d; z++) { expandLevel(o->fmt, src + z * slice, w, h, tmp); all.insert(all.end(), tmp.begin(), tmp.end()); }
						glTexImage3D(GL_TEXTURE_3D, l, GL_RGBA8, w, h, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, all.data());
					}
					else
					{
						expandLevel(o->fmt, src, w, h, tmp);
						glTexImage2D(tgt, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
					}
				}
				else if (o->iface == IVolume) glTexImage3D(GL_TEXTURE_3D, l, tf.internal, w, h, d, 0, tf.format, tf.type, src);
				else
				{
					glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
					glTexImage2D(tgt, l, tf.internal, w, h, 0, tf.format, tf.type, src);
				}
			}
	}
	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, o->levels - 1);
	if (tf.swizzle)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, tf.swz[0]); glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, tf.swz[1]);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, tf.swz[2]); glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, tf.swz[3]);
	}
	o->glVer = o->ver;
}

void createRtStorage(Obj* o)   // color or depth render target texture
{
	TexFmt tf = texFmtOf(o->fmt);
	glBindTexture(GL_TEXTURE_2D, o->glTex);
	if (tf.depth) glTexImage2D(GL_TEXTURE_2D, 0, tf.internal, o->w, o->h, 0, tf.format, tf.type, nullptr);
	else if (tf.convert || tf.compressed || tf.none) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, o->w, o->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	else glTexImage2D(GL_TEXTURE_2D, 0, tf.internal, o->w, o->h, 0, tf.format, tf.type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, o->levels > 1 ? o->levels - 1 : 0);
	if (o->levels > 1)
	{
		// allocate the remaining levels so that mip-mapped sampling is complete
		for (uint32_t l = 1; l < o->levels; l++)
		{
			uint32_t w = std::max(1u, o->w >> l), h = std::max(1u, o->h >> l);
			if (tf.depth) glTexImage2D(GL_TEXTURE_2D, l, tf.internal, w, h, 0, tf.format, tf.type, nullptr);
			else if (tf.convert || tf.compressed || tf.none) glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			else glTexImage2D(GL_TEXTURE_2D, l, tf.internal, w, h, 0, tf.format, tf.type, nullptr);
		}
	}
	if (tf.swizzle)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, tf.swz[0]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, tf.swz[1]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, tf.swz[2]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, tf.swz[3]);
	}
	if (tf.depth) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE); }
}

// Texture object that holds the pixels of `o` (a surface resolves to its parent texture).
Obj* texOwner(Obj* o) { if (o->iface == ISurface && o->parent) if (Obj* p = objectAt(o->parent)) return p; return o; }

GLuint ensureTexture(Obj* o0)
{
	Obj* o = texOwner(o0);
	o->lastUse = o0->lastUse = gFrameNo;
	// A stand-alone surface (e.g. the implicit depth/stencil buffer) can only get here as a framebuffer
	// attachment: it has no mip table to upload from, so give it render-target storage.
	if (o->iface == ISurface && !o->rtTex && o->levelOff.empty())
	{
		static int n = 0;
		if (n++ < 4) log("GL: stand-alone surface %ux%u fmt %u usage %x used as attachment", o->w, o->h, o->fmt, o->usage);
		o->rtTex = true;
	}
	if (!o->glTex)
	{
		glGenTextures(1, &o->glTex);
		o->glVer = 0;
		if (o->rtTex && (o->iface == ITexture || o->iface == ISurface)) { createRtStorage(o); o->glVer = o->ver; }
		if (o->rtTex && o->iface == ICube)
		{
			// render-target cube map (environment reflections): allocate all six faces
			TexFmt tf = texFmtOf(o->fmt);
			bool plain = !(tf.convert || tf.compressed || tf.none || tf.depth);
			glBindTexture(GL_TEXTURE_CUBE_MAP, o->glTex);
			for (uint32_t l = 0; l < std::max(1u, o->levels); l++)
				for (uint32_t f = 0; f < 6; f++)
				{
					uint32_t w = std::max(1u, o->w >> l);
					if (plain) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, tf.internal, w, w, 0, tf.format, tf.type, nullptr);
					else glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGBA8, w, w, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, o->levels > 1 ? o->levels - 1 : 0);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			o->glVer = o->ver;
			log("GL: cube render target %ux%u fmt %u levels %u", o->w, o->w, o->fmt, o->levels);
		}
	}
	if (!o->rtTex && o->glVer != o->ver && o->data) uploadTexture(o);
	return o->glTex;
}

uint64_t gUpUs, gUpMaxSize, gUpDyn;
uint64_t gUpBytes, gUpFull, gUpSub, gUpCnt;
GLuint ensureBuffer(Obj* o, GLenum target)
{
	o->lastUse = gFrameNo;
	if (!o->data) { if (!o->glBuf) glGenBuffers(1, &o->glBuf); return o->glBuf; }
	if (!o->glBuf) { glGenBuffers(1, &o->glBuf); o->glVer = 0; }
	if (o->glVer != o->ver)
	{
		uint64_t tu0 = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		gUpMaxSize = std::max<uint64_t>(gUpMaxSize, o->size); if (o->usage & 0x200) gUpDyn++;
		glBindBuffer(target, o->glBuf);
		if (o->glVer && o->glSize == o->size && o->dirtyLo < o->dirtyHi)
			{ glBufferSubData(target, o->dirtyLo, o->dirtyHi - o->dirtyLo, (const uint8_t*)guest::host(o->data) + o->dirtyLo); gUpBytes += o->dirtyHi - o->dirtyLo; gUpSub++; }
		else
		{
			glBufferData(target, o->size, guest::host(o->data), GL_DYNAMIC_DRAW);
			gUpBytes += o->size; gUpFull++;
			o->glSize = o->size;
		}
		o->dirtyLo = 0xffffffffu; o->dirtyHi = 0;
		o->glVer = o->ver;
		gUpUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - tu0;
	}
	return o->glBuf;
}

// ---------------------------------------------------------------------------------------------------------------
// render targets
bool isBack(Obj* rt) { return !rt || rt == gBackBuffer; }

void targetSize(Obj* rt, uint32_t& w, uint32_t& h)
{
	if (isBack(rt)) { w = backW; h = backH; return; }
	Obj* t = texOwner(rt);
	uint32_t l = rt->iface == ISurface ? rt->level : 0;
	w = std::max(1u, t->w >> l); h = std::max(1u, t->h >> l);
}

// Device Reset to another size: the back buffer texture and its depth/stencil buffer are re-allocated
std::atomic<bool> gResizePending{false};
void resizeBackBufferImpl()
{
	if (!gResizePending.exchange(false)) return;
	if (!gInit || !gBackTex) return;
	GLint prev = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
	glBindTexture(GL_TEXTURE_2D, gBackTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, backW, backH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
	glBindRenderbuffer(GL_RENDERBUFFER, gBackDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, backW, backH);
	glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gBackTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gBackDepth);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (gBackBuffer) { gBackBuffer->w = backW; gBackBuffer->h = backH; }
	log("GL: back buffer resized to %ux%u (status 0x%x)", backW, backH, glCheckFramebufferStatus(GL_FRAMEBUFFER));
}

void bindTargets()
{
	resizeBackBufferImpl();
	Obj* rt = dev.rt0;
	if (isBack(rt))
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo);
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, gDrawFbo);
	GLuint tex = ensureTexture(rt);
	uint32_t lvl = rt->iface == ISurface ? rt->level : 0;
	Obj* owner = texOwner(rt);
	GLenum faceTarget = owner->iface == ICube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (rt->iface == ISurface ? rt->face : 0) : GL_TEXTURE_2D;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, faceTarget, tex, lvl);
	Obj* ds = dev.ds;
	if (ds && ds->fmt != 0x4c4c554e)
	{
		TexFmt tf = texFmtOf(ds->fmt);
		GLuint dt = ensureTexture(ds);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, tf.depth ? dt : 0, 0);
	}
	else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	static int logged = 0;
	if (logged < 6)
	{
		GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (st != GL_FRAMEBUFFER_COMPLETE) { log("GL: render target framebuffer incomplete (0x%x): fmt %u %ux%u ds %u", st, rt->fmt, rt->w, rt->h, ds ? ds->fmt : 0); logged++; }
	}
}

// ---------------------------------------------------------------------------------------------------------------
// programs
struct Prog
{
	GLuint id = 0;
	bool failed = false;
	GLint vc = -1, vci = -1, vcb = -1, pc = -1, pci = -1, pcb = -1, flip = -1, bgra = -1, at = -1, s[16];
};
std::unordered_map<uint64_t, Prog> gProgs;

Prog* getProgram(ShaderObj* vs, ShaderObj* ps)
{
	uint64_t key = vs->hash * 1000003ull ^ (ps->hash + 0x9e3779b97f4a7c15ull + (vs->hash << 6));
	auto it = gProgs.find(key);
	if (it != gProgs.end()) return it->second.failed ? nullptr : &it->second;
	Prog& p = gProgs[key];
	p.failed = true;
	if (!vs->ok || !ps->ok || !vs->info.isVs || ps->info.isVs) return nullptr;
	std::string vsrc = shdr::compose(vs->info), psrc = shdr::compose(ps->info, &vs->info.outputs);
	GLuint v = compileStage(GL_VERTEX_SHADER, vsrc, "vertex");
	GLuint f = compileStage(GL_FRAGMENT_SHADER, psrc, "pixel");
	if (!v || !f)
	{
		static int dumped = 0;
		if (dumped++ < 3) log("GL: failing sources:\n%s\n----\n%s", vsrc.c_str(), psrc.c_str());
		return nullptr;
	}
	p.id = glCreateProgram();
	glAttachShader(p.id, v);
	glAttachShader(p.id, f);
	for (auto& a : vs->info.inputs) glBindAttribLocation(p.id, a.second, a.first.c_str());
	glLinkProgram(p.id);
	glDeleteShader(v);
	glDeleteShader(f);
	GLint ok = 0;
	glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char buf[1024];
		glGetProgramInfoLog(p.id, sizeof buf, nullptr, buf);
		log("GL: program link failed: %s", buf);
		return nullptr;
	}
	p.failed = false;
	p.vc = glGetUniformLocation(p.id, "vc"); p.vci = glGetUniformLocation(p.id, "vci"); p.vcb = glGetUniformLocation(p.id, "vcb");
	p.pc = glGetUniformLocation(p.id, "pc"); p.pci = glGetUniformLocation(p.id, "pci"); p.pcb = glGetUniformLocation(p.id, "pcb");
	p.flip = glGetUniformLocation(p.id, "u_flip"); p.bgra = glGetUniformLocation(p.id, "u_bgra"); p.at = glGetUniformLocation(p.id, "u_at");
	for (int i = 0; i < 16; i++) { char n[8]; snprintf(n, sizeof n, "s%d", i); p.s[i] = glGetUniformLocation(p.id, n); }
	static int linked = 0;
	if (linked++ < 8) log("GL: linked program #%d (vs %016llx ps %016llx)", linked, (unsigned long long)vs->hash, (unsigned long long)ps->hash);
	return &p;
}

// ---------------------------------------------------------------------------------------------------------------
// state mapping
GLenum cmpFunc(uint32_t f)
{
	switch (f)
	{
	case 1: return GL_NEVER; case 2: return GL_LESS; case 3: return GL_EQUAL; case 4: return GL_LEQUAL;
	case 5: return GL_GREATER; case 6: return GL_NOTEQUAL; case 7: return GL_GEQUAL; default: return GL_ALWAYS;
	}
}
GLenum blendFactor(uint32_t b)
{
	switch (b)
	{
	case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
	case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
	case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
	case 14: return GL_CONSTANT_COLOR; case 15: return GL_ONE_MINUS_CONSTANT_COLOR; default: return GL_ONE;
	}
}
GLenum blendOp(uint32_t o)
{
	switch (o) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; default: return GL_FUNC_ADD; }
}
GLenum stencilOp(uint32_t o)
{
	switch (o) { case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR; case 5: return GL_DECR; case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP; default: return GL_KEEP; }
}
GLenum wrapMode(uint32_t a)
{
	switch (a) { case 2: case 5: return GL_MIRRORED_REPEAT; case 3: case 4: return GL_CLAMP_TO_EDGE; default: return GL_REPEAT; }
}

void applyState(uint32_t tw, uint32_t th)
{
	const uint32_t* rs = dev.rs;
	bool hasDepth = isBack(dev.rt0) || (dev.ds && dev.ds->fmt != 0x4c4c554e);
	if (rs[7] && hasDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	glDepthFunc(cmpFunc(rs[23]));
	glDepthMask(rs[14] && hasDepth ? GL_TRUE : GL_FALSE);
	switch (rs[22])
	{
	case 2: glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); break;
	case 3: glEnable(GL_CULL_FACE); glCullFace(GL_BACK); break;
	default: glDisable(GL_CULL_FACE); break;
	}
	glFrontFace(GL_CCW);   // y is flipped in the vertex shader, so D3D's clockwise front faces become counter-clockwise
	if (rs[27])
	{
		glEnable(GL_BLEND);
		if (rs[206]) glBlendFuncSeparate(blendFactor(rs[19]), blendFactor(rs[20]), blendFactor(rs[207]), blendFactor(rs[208]));
		else glBlendFunc(blendFactor(rs[19]), blendFactor(rs[20]));
		if (rs[206]) glBlendEquationSeparate(blendOp(rs[171]), blendOp(rs[209])); else glBlendEquation(blendOp(rs[171]));
		uint32_t bf = rs[193];
		glBlendColor(((bf >> 16) & 255) / 255.f, ((bf >> 8) & 255) / 255.f, (bf & 255) / 255.f, ((bf >> 24) & 255) / 255.f);
	}
	else glDisable(GL_BLEND);
	uint32_t cw = rs[168];
	glColorMask(cw & 1, cw & 2, cw & 4, cw & 8);
	if (rs[52] && hasDepth)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(cmpFunc(rs[56]), rs[57] & 255, rs[58] & 255);
		glStencilOp(stencilOp(rs[53]), stencilOp(rs[54]), stencilOp(rs[55]));
		glStencilMask(rs[59] & 255);
	}
	else glDisable(GL_STENCIL_TEST);
	float bias, slope;
	memcpy(&bias, &rs[195], 4);
	memcpy(&slope, &rs[175], 4);
	if (bias != 0.f || slope != 0.f) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(slope, bias * 16777216.f); }
	else glDisable(GL_POLYGON_OFFSET_FILL);
	uint32_t vx = dev.vp[0], vy = dev.vp[1], vw = dev.vp[2], vh = dev.vp[3];
	if (!vw || !vh) { vx = vy = 0; vw = tw; vh = th; }
	glViewport(vx, vy, vw, vh);
	glDepthRangef(dev.vpZ[0], dev.vpZ[1]);
	if (rs[174] && dev.scissor[2] > dev.scissor[0])
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(dev.scissor[0], dev.scissor[1], dev.scissor[2] - dev.scissor[0], dev.scissor[3] - dev.scissor[1]);
	}
	else glDisable(GL_SCISSOR_TEST);
}

GLuint dummyTex(GLenum target)
{
	GLuint& t = target == GL_TEXTURE_CUBE_MAP ? gDummyCube : target == GL_TEXTURE_3D ? gDummy3D : gDummy2D;
	if (!t)
	{
		glGenTextures(1, &t);
		glBindTexture(target, t);
		uint8_t px[4] = {0, 0, 0, 255};
		if (target == GL_TEXTURE_CUBE_MAP) for (int f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		else if (target == GL_TEXTURE_3D) glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	return t;
}

void bindSamplers(Prog* p, ShaderObj* ps)
{
	for (int i = 0; i < 16; i++)
	{
		int st = ps->info.samplerType[i];
		{
			static int nl3 = 0;
			if ((st || p->s[i] >= 0) && nl3++ < 40) log("GL: sampler slot %d psType %d loc %d", i, st, p->s[i]);
		}
		if (!st || p->s[i] < 0) continue;
		GLenum target = st == 3 ? GL_TEXTURE_CUBE_MAP : st == 4 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
		glActiveTexture(GL_TEXTURE0 + i);
		Obj* t = dev.tex[i];
		{
			static int nl2 = 0;
			if (nl2++ < 40) log("GL: ps sampler %d type %d tex %p (iface %d) loc %d", i, st, (void*)t, t ? (int)texOwner(t)->iface : -1, p->s[i]);
		}
		GLuint name = 0;
		bool noFilter = false;
		if (t && targetOf(texOwner(t)) == target)
		{
			name = ensureTexture(t);
			{
				static int nlog = 0;
				Obj* ow = texOwner(t);
				if (nlog++ < 60)
				{
					const uint8_t* px = ow->data ? (const uint8_t*)guest::host(ow->data + ow->levelOff[0]) : nullptr;
					log("GL: sampler %d tex fmt %u %ux%u lv %u usage %x rt %d ver %u gl %u px %02x %02x %02x %02x", i, ow->fmt, ow->w, ow->h, ow->levels, ow->usage, (int)ow->rtTex, ow->ver, name,
					    px ? px[0] : 0, px ? px[1] : 0, px ? px[2] : 0, px ? px[3] : 0);
				}
			}
			noFilter = texFmtOf(texOwner(t)->fmt).floatNoFilter;
		}
		glBindTexture(target, name ? name : dummyTex(target));
		glUniform1i(p->s[i], i);
		if (!gSamplers[i]) glGenSamplers(1, &gSamplers[i]);
		const uint32_t* s = dev.samp[i];
		glSamplerParameteri(gSamplers[i], GL_TEXTURE_WRAP_S, wrapMode(s[1]));
		glSamplerParameteri(gSamplers[i], GL_TEXTURE_WRAP_T, wrapMode(s[2]));
		glSamplerParameteri(gSamplers[i], GL_TEXTURE_WRAP_R, wrapMode(s[3]));
		uint32_t mag = s[5], mn = s[6], mip = s[7];
		bool hasMips = t && texOwner(t)->levels > 1;
		GLenum minF;
		if (noFilter) minF = GL_NEAREST;
		else if (mn == 1) minF = (!hasMips || mip == 0) ? GL_NEAREST : mip == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
		else minF = (!hasMips || mip == 0) ? GL_LINEAR : mip == 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
		glSamplerParameteri(gSamplers[i], GL_TEXTURE_MIN_FILTER, minF);
		glSamplerParameteri(gSamplers[i], GL_TEXTURE_MAG_FILTER, (noFilter || mag == 1) ? GL_NEAREST : GL_LINEAR);
		glBindSampler(i, gSamplers[i]);
	}
	glActiveTexture(GL_TEXTURE0);
}

struct DeclElem { uint16_t stream, offset; uint8_t type, method, usage, index; };

bool attribFormat(uint8_t type, GLint& size, GLenum& gtype, bool& norm)
{
	switch (type)
	{
	case 0: size = 1; gtype = GL_FLOAT; norm = false; return true;
	case 1: size = 2; gtype = GL_FLOAT; norm = false; return true;
	case 2: size = 3; gtype = GL_FLOAT; norm = false; return true;
	case 3: size = 4; gtype = GL_FLOAT; norm = false; return true;
	case 4: size = 4; gtype = GL_UNSIGNED_BYTE; norm = true; return true;     // D3DCOLOR (swizzled in the shader)
	case 5: size = 4; gtype = GL_UNSIGNED_BYTE; norm = false; return true;    // UBYTE4
	case 6: size = 2; gtype = GL_SHORT; norm = false; return true;
	case 7: size = 4; gtype = GL_SHORT; norm = false; return true;
	case 8: size = 4; gtype = GL_UNSIGNED_BYTE; norm = true; return true;     // UBYTE4N
	case 9: size = 2; gtype = GL_SHORT; norm = true; return true;
	case 10: size = 4; gtype = GL_SHORT; norm = true; return true;
	case 11: size = 2; gtype = GL_UNSIGNED_SHORT; norm = true; return true;
	case 12: size = 4; gtype = GL_UNSIGNED_SHORT; norm = true; return true;
	case 13: size = 4; gtype = GL_UNSIGNED_INT_2_10_10_10_REV; norm = false; return true;
	case 14: size = 4; gtype = GL_INT_2_10_10_10_REV; norm = true; return true;
	case 15: size = 2; gtype = GL_HALF_FLOAT; norm = false; return true;
	case 16: size = 4; gtype = GL_HALF_FLOAT; norm = false; return true;
	}
	return false;
}

int primVertices(uint32_t prim, uint32_t n)
{
	switch (prim) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; }
	return 0;
}
GLenum primMode(uint32_t prim)
{
	switch (prim) { case 1: return GL_POINTS; case 2: return GL_LINES; case 3: return GL_LINE_STRIP; case 5: return GL_TRIANGLE_STRIP; case 6: return GL_TRIANGLE_FAN; default: return GL_TRIANGLES; }
}

const char* kBlitVs = "#version 300 es\nout vec2 uv;\nvoid main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); uv = vec2(p.x, 1.0 - p.y); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
const char* kBlitFs = "#version 300 es\nprecision mediump float;\nin vec2 uv;\nuniform sampler2D t;\nout vec4 o;\nvoid main(){ o = vec4(texture(t, uv).rgb, 1.0); }\n";
}   // namespace
void resizeBackBuffer() { gResizePending = true; }   // may be called from a thread without the GL context: applied by the GL thread

bool init()
{
	if (gInit) return gOk;
	gInit = true;
	if (!gfx::ready() || !gfx::ownedByGame()) { log("GL: no context for the game thread - drawing disabled"); return false; }
	const char* ext = (const char*)glGetString(GL_EXTENSIONS);
	gHasS3tc = ext && strstr(ext, "GL_EXT_texture_compression_s3tc");
	log("GL: %s | %s | s3tc %d | float rt %d", glGetString(GL_RENDERER), glGetString(GL_VERSION), (int)gHasS3tc, ext && strstr(ext, "EXT_color_buffer_float") != nullptr);
	glGenVertexArrays(1, &gVao);
	glGenFramebuffers(1, &gBackFbo);
	glGenFramebuffers(1, &gDrawFbo);
	glGenFramebuffers(1, &gReadFbo);
	glGenTextures(1, &gBackTex);
	glBindTexture(GL_TEXTURE_2D, gBackTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, backW, backH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenRenderbuffers(1, &gBackDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, gBackDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, backW, backH);
	glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gBackTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gBackDepth);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	log("GL: back buffer %ux%u framebuffer status 0x%x", backW, backH, st);
	GLuint v = compileStage(GL_VERTEX_SHADER, kBlitVs, "blit vs"), f = compileStage(GL_FRAGMENT_SHADER, kBlitFs, "blit fs");
	gBlitProg = glCreateProgram();
	glAttachShader(gBlitProg, v);
	glAttachShader(gBlitProg, f);
	glLinkProgram(gBlitProg);
	glGenBuffers(1, &gUpVbo);
	glGenBuffers(1, &gUpIbo);
	glErr("init");
	gOk = st == GL_FRAMEBUFFER_COMPLETE;
	return gOk;
}

// The GL context is bound lazily to the first thread that actually renders (the game creates the device on one thread and draws on another).
bool ensureGl()
{
	if (!gInit)
	{
		bool a = gfx::acquire();
		log("GL: first rendering call on tid %d (acquire %d)", (int)gettid(), (int)a);
		init();
	}
	else if (gOk && !gfx::isOwner()) gfx::acquire();   // the game moved rendering to another thread (e.g. fullscreen video)
	return gOk;
}
bool ready() { return gOk; }

void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
	if (!ensureGl()) return;
	bindTargets();
	uint32_t tw, th;
	targetSize(dev.rt0, tw, th);
	glDisable(GL_SCISSOR_TEST);
	GLbitfield m = 0;
	if (flags & 1) { glColorMask(1, 1, 1, 1); glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f); m |= GL_COLOR_BUFFER_BIT; }
	bool hasDepth = isBack(dev.rt0) || dev.ds;
	if ((flags & 2) && hasDepth) { glDepthMask(GL_TRUE); glClearDepthf(z); m |= GL_DEPTH_BUFFER_BIT; }
	if ((flags & 4) && hasDepth) { glStencilMask(0xff); glClearStencil((GLint)stencil); m |= GL_STENCIL_BUFFER_BIT; }
	if (!m) return;
	if (count && rects)
	{
		glEnable(GL_SCISSOR_TEST);
		for (uint32_t i = 0; i < count; i++)
		{
			int32_t x1 = guest::load<int32_t>(rects + 16 * i), y1 = guest::load<int32_t>(rects + 16 * i + 4), x2 = guest::load<int32_t>(rects + 16 * i + 8), y2 = guest::load<int32_t>(rects + 16 * i + 12);
			glScissor(x1, y1, x2 - x1, y2 - y1);
			glClear(m);
		}
		glDisable(GL_SCISSOR_TEST);
	}
	else glClear(m);
	static int n = 0;
	if (n++ < 4) log("d3d9: Clear flags %x color %08x z %g -> %s", flags, color, (double)z, isBack(dev.rt0) ? "back buffer" : "render target");
}

// Streaming rings for dynamic buffers: the game Locks a dynamic buffer for every draw, so writing into the
// one GL buffer stalls the driver on draws still in flight. Each draw instead copies just the range it uses
// to a fresh spot in a ring (orphaned when full), which never touches data an earlier draw reads.
static GLuint gRingVbo = 0, gRingIbo = 0;
static uint32_t gRingVPos = 0, gRingIPos = 0;
static const uint32_t kRingV = 32u << 20, kRingI = 8u << 20;
static uint64_t gRingUs, gRingOrph, gRingBytes, gRingCalls, gRingNs;
static inline uint64_t ringNow() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
typedef void (*PFN_bufStorage)(GLenum, GLsizeiptr, const void*, GLbitfield);
struct PRing
{
	uint8_t* ptr = nullptr;
	GLsync fence[4] = {};
	int seg = 0;
	bool tried = false, ok = false;
};
static PRing gPV, gPI;
static PFN_bufStorage pBufStorage = nullptr;
static int64_t ringPut(GLenum target, GLuint& buf, uint32_t& pos, uint32_t cap, const void* head, uint32_t headBytes, const void* data, uint32_t bytes)
{
	uint32_t total = headBytes + bytes;
	if (total > cap / 2) { gRingNs++; return -1; }
	uint64_t rt0 = ringNow(); gRingCalls++; gRingBytes += bytes;
	PRing& pr = (target == GL_ARRAY_BUFFER) ? gPV : gPI;
	if (!buf)
	{
		glGenBuffers(1, &buf); glBindBuffer(target, buf);
		if (!pr.tried)
		{
			// try the persistent path: the buffer name is replaced inside
			const char* ext = (const char*)glGetString(GL_EXTENSIONS);
			if (ext && strstr(ext, "GL_EXT_buffer_storage"))
			{
				if (!pBufStorage) pBufStorage = (PFN_bufStorage)eglGetProcAddress("glBufferStorageEXT");
				if (pBufStorage)
				{
					const GLbitfield fl = 0x0002 | 0x0040 | 0x0080;
					GLuint nb; glGenBuffers(1, &nb); glBindBuffer(target, nb);
					pBufStorage(target, cap, nullptr, fl);
					uint8_t* p = (uint8_t*)glMapBufferRange(target, 0, cap, fl);
					if (p && glGetError() == GL_NO_ERROR) { glDeleteBuffers(1, &buf); buf = nb; pr.ptr = p; pr.ok = true; log("GL: persistent ring %u KB target %x", cap >> 10, target); }
					else { glDeleteBuffers(1, &nb); log("GL: persistent mapping failed"); }
				}
			}
			pr.tried = true;
		}
		if (!pr.ok) { glBindBuffer(target, buf); glBufferData(target, cap, nullptr, GL_STREAM_DRAW); }
		pos = 0;
	}
	glBindBuffer(target, buf);
	pos = (pos + 15) & ~15u;
	if (pr.ok)
	{
		const uint32_t segSz = cap / 4;
		if (pos + total > cap) pos = 0;
		// entering a new segment: fence the one we left, wait for the one we enter
		int segNow = (int)((pos + total - 1) / segSz);
		if (segNow != pr.seg)
		{
			if (pr.fence[pr.seg]) glDeleteSync(pr.fence[pr.seg]);
			pr.fence[pr.seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			if (pr.fence[segNow])
			{
				glClientWaitSync(pr.fence[segNow], GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull);
				glDeleteSync(pr.fence[segNow]);
				pr.fence[segNow] = nullptr;
			}
			pr.seg = segNow;
		}
		uint32_t at = pos + headBytes;
		if (bytes) memcpy(pr.ptr + at, data, bytes);
		pos += total;
		gRingUs += ringNow() - rt0;
		return at;
	}
	if (pos + total > cap) { glBufferData(target, cap, nullptr, GL_STREAM_DRAW); pos = 0; gRingOrph++; }
	uint32_t at = pos + headBytes;
	if (bytes)
	{
		void* m = glMapBufferRange(target, at, bytes, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
		if (m) { memcpy(m, data, bytes); glUnmapBuffer(target); }
		else glBufferSubData(target, at, bytes, data);
	}
	pos += total;
	gRingUs += ringNow() - rt0;
	(void)head;
	return at;
}

#include <chrono>
static inline uint64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static uint64_t tProg, tState, tUni, tSamp, tAttr, tDraw, nDraw;

static bool gDiagPair = false;   // set per draw: first draws of each new shader pair during gameplay
static bool isDiagFrame() { return verbose() && (gFrameNo == 700 || gFrameNo == 1800 || gFrameNo == 3600 || gDiagPair); }
static void dumpBmp(const char* path, uint32_t w, uint32_t h)
{
	if (!w || !h || w > 4096 || h > 4096) return;
	std::vector<uint8_t> px((size_t)w * h * 4);
	glFinish();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	// half size 24-bit BMP, bottom-up as read
	uint32_t ow = w / 2, oh = h / 2, rowB = (ow * 3 + 3) & ~3u, imgB = rowB * oh;
	std::vector<uint8_t> bmp(54 + imgB, 0);
	auto w32 = [&](size_t o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
	bmp[0] = 'B'; bmp[1] = 'M'; w32(2, 54 + imgB); w32(10, 54); w32(14, 40); w32(18, ow); w32(22, oh); bmp[26] = 1; bmp[28] = 24; w32(34, imgB);
	for (uint32_t y = 0; y < oh; y++)
		for (uint32_t x = 0; x < ow; x++)
		{
			const uint8_t* q = &px[((size_t)(2 * y) * w + 2 * x) * 4];
			uint8_t* o = &bmp[54 + (size_t)y * rowB + x * 3];
			o[0] = q[2]; o[1] = q[1]; o[2] = q[0];
		}
	if (FILE* f = fopen(path, "wb")) { fwrite(bmp.data(), 1, bmp.size(), f); fclose(f); log("dumped %s (%ux%u)", path, ow, oh); }
	else log("dump failed: %s", path);
}

void draw(const DrawCall& dc)
{
	{ static int n = 0; if (n++ < 5) log("GL: draw on tid %d ctx %p", (int)gettid(), (void*)eglGetCurrentContext()); }
	if (!ensureGl()) return;
	ShaderObj *vs = dev.vs, *ps = dev.ps;
	static int noShader = 0;
	if (!vs || !ps)
	{
		if (noShader++ < 5) log("d3d9: draw without shaders (fixed function not implemented): fvf %x vdecl %p", dev.fvf, (void*)dev.vdecl);
		return;
	}
	uint64_t t0 = nowUs();
	Prog* p = getProgram(vs, ps);
	if (!p) { static int nf = 0; if (nf++ < 10) log("d3d9: draw skipped: no program for vs %016llx ps %016llx", (unsigned long long)vs->hash, (unsigned long long)ps->hash); return; }
	if (verbose())
	{
		// diagnostics: which shader pairs / blend states are drawn (dumped as a histogram)
		struct Key { uint64_t v, p; uint32_t st; bool operator<(const Key& o) const { return std::tie(v, p, st) < std::tie(o.v, o.p, o.st); } };
		static std::map<Key, std::pair<uint32_t, uint64_t>> hist;
		static uint32_t lastDump = 0;
		uint32_t st = (dev.rs[27] ? 1u : 0u) | (dev.rs[7] ? 2u : 0u) | (dev.rs[14] ? 4u : 0u) | (dev.rs[19] & 15) << 4 | (dev.rs[20] & 15) << 8 | (dc.prim & 7) << 12 | (dev.rt0 ? 1u << 16 : 0u);
		auto& h = hist[Key{vs->hash, ps->hash, st}];
		h.first++; h.second += dc.primCount;
		if (gFrameNo >= lastDump + 1800 && gFrameNo > 0)
		{
			lastDump = gFrameNo;
			std::vector<std::pair<std::pair<uint32_t, uint64_t>, Key>> v;
			for (auto& kv : hist) v.push_back({kv.second, kv.first});
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first.second > b.first.second; });
			log("DRAWHIST frame %u: %zu combos", gFrameNo, v.size());
			for (size_t i = 0; i < v.size() && i < 40; i++)
				log("DRAWHIST vs %016llx ps %016llx blend %u z %u zw %u src %u dst %u prim %u rt %u : draws %u prims %llu", (unsigned long long)v[i].second.v, (unsigned long long)v[i].second.p,
					v[i].second.st & 1, v[i].second.st >> 1 & 1, v[i].second.st >> 2 & 1, v[i].second.st >> 4 & 15, v[i].second.st >> 8 & 15, v[i].second.st >> 12 & 7, v[i].second.st >> 16 & 1, v[i].first.first, (unsigned long long)v[i].first.second);
			hist.clear();
		}
	}
	{
		static std::map<std::tuple<uint64_t, uint64_t, uint32_t>, int> seen;
		static int logged = 0;
		uint32_t bl = (dev.rs[27] ? 1u : 0u) | (dev.rs[19] & 15) << 4 | (dev.rs[20] & 15) << 8;
		gDiagPair = false;
		static bool diagOn = access("/sdcard/ra3_diag", F_OK) == 0;
		if (diagOn && gFrameNo >= 3000 && logged < 120)
		{
			int& n = seen[{vs->hash, ps->hash, bl}];
			if (n < 2) { n++; logged++; gDiagPair = true; log("DIAGPAIR vs %016llx ps %016llx bl %x", (unsigned long long)vs->hash, (unsigned long long)ps->hash, bl); }
		}
	}
	if (isDiagFrame())
	{
		static bool srcDumped = false;
		if (!srcDumped)
		{
			srcDumped = true;
			std::string vsrc = shdr::compose(vs->info), psrc = shdr::compose(ps->info, &vs->info.outputs);
			log("DD VS source:\n%s", vsrc.c_str());
			log("DD PS source:\n%s", psrc.c_str());
		}
	}
	if (gDiagPair && dc.primCount >= 500)
	{
		static int srcN = 0;
		if (srcN++ < 8)
		{
			log("DIAGPAIR VS source:\n%s", shdr::compose(vs->info).c_str());
			log("DIAGPAIR PS source:\n%s", shdr::compose(ps->info, &vs->info.outputs).c_str());
		}
	}
	if (isDiagFrame())
	{
		static int dn = 0;
		std::string tx;
		for (int i = 0; i < 4; i++)
			if (Obj* t = dev.tex[i])
			{
				unsigned nz = 0;
				if (t->data && t->size) { const uint8_t* b = (const uint8_t*)guest::host(t->data); for (uint32_t k = 0; k < std::min<uint32_t>(t->size, 4096); k += 7) nz += b[k] != 0; }
				char b2[96]; snprintf(b2, sizeof b2, " t%d=%ux%u/f%x/l%u/nz%u%s", i, t->w, t->h, t->fmt, t->levels, nz, t->rtTex ? "/RT" : "");
				tx += b2;
			}
		if (dev.vdecl)
		{
			std::string de;
			for (size_t i = 0; i < dev.vdecl->blob.size() / 8; i++)
			{
				DeclElem e; memcpy(&e, dev.vdecl->blob.data() + 8 * i, 8);
				if (e.stream == 0xff) break;
				char b3[48]; snprintf(b3, sizeof b3, " s%u+%u:t%u:u%u.%u", e.stream, e.offset, e.type, e.usage, e.index); de += b3;
			}
			Stream& st0 = dev.streams[0];
			std::string v0;
			if (st0.vb && st0.vb->data && st0.stride)
			{
				const float* f = (const float*)((const uint8_t*)guest::host(st0.vb->data) + st0.offset);
				for (uint32_t i = 0; i < std::min<uint32_t>(st0.stride / 4 * 2, 16); i++) { char b4[24]; snprintf(b4, sizeof b4, " %g", (double)f[i]); v0 += b4; }
			}
			log("DD   decl%s stride %u v:%s", de.c_str(), st0.stride, v0.c_str());
		}
		if (dn < 2)
			for (auto& cc : vs->info.ctab)
			{
				if (cc.regSet != 2) continue;
				std::string vv;
				for (uint32_t r = 0; r < std::min<uint32_t>(cc.regCount, 4); r++)
					{ char b5[96]; const float* f = dev.vsF + 4 * (cc.regIndex + r); snprintf(b5, sizeof b5, " [%g %g %g %g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3]); vv += b5; }
				log("DD   VS c %s r%u x%u:%s", cc.name.c_str(), cc.regIndex, cc.regCount, vv.c_str());
			}
		if (dn < 8 && dev.streams[0].vb && dev.streams[0].vb->data && dev.streams[0].stride)
		{
			Stream& s0 = dev.streams[0];
			std::string o;
			for (uint32_t k = 0; k < 6; k++)
			{
				int64_t vi = dc.baseVertex + (dc.indexed ? 0 : 0);
				if (dc.indexed && dev.ib && dev.ib->data)
				{
					const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data);
					vi += dev.ib->fmt == 102 ? ((const uint32_t*)ip)[dc.startIndex + k] : ((const uint16_t*)ip)[dc.startIndex + k];
				}
				else vi += k;
				const float* f = (const float*)((const uint8_t*)guest::host(s0.vb->data) + s0.offset + (uint64_t)vi * s0.stride);
				const float* m = dev.vsF + 44;   // World at r11 (checked above)
				float cx = f[0] * m[0] + f[1] * m[4] + f[2] * m[8] + m[12], cy = f[0] * m[1] + f[1] * m[5] + f[2] * m[9] + m[13];
				float cz = f[0] * m[2] + f[1] * m[6] + f[2] * m[10] + m[14], cw = f[0] * m[3] + f[1] * m[7] + f[2] * m[11] + m[15];
				char b6[160]; snprintf(b6, sizeof b6, " v%lld(%g %g %g)->clip(%g %g %g %g)", (long long)vi, (double)f[0], (double)f[1], (double)f[2], (double)cx, (double)cy, (double)cz, (double)cw);
				o += b6;
			}
			log("DD   verts%s", o.c_str());
		}
		if (dev.rs[7] && dev.streams[0].vb && dev.streams[0].vb->data && dev.streams[0].stride)
		{
			static uint32_t zf = 0; static int zd = 0; static std::set<const void*> zseen;
			if (zf != gFrameNo) { zf = gFrameNo; zd = 0; }
			if (zd++ < 4)
			{
				if (zseen.insert(vs).second) log("DD3 VS source:\n%s", shdr::compose(vs->info).c_str());
				for (auto& cc : vs->info.ctab)
				{
					if (cc.regSet != 2) continue;
					std::string vv;
					for (uint32_t r = 0; r < std::min<uint32_t>(cc.regCount, 4); r++)
						{ char b5[96]; const float* f = dev.vsF + 4 * (cc.regIndex + r); snprintf(b5, sizeof b5, " [%g %g %g %g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3]); vv += b5; }
					log("DD3   c %s r%u x%u:%s", cc.name.c_str(), cc.regIndex, cc.regCount, vv.c_str());
				}
				Stream& s0 = dev.streams[0];
				std::string o;
				for (uint32_t k = 0; k < 3; k++)
				{
					int64_t vi = dc.baseVertex;
					if (dc.indexed && dev.ib && dev.ib->data)
					{
						const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data);
						vi += dev.ib->fmt == 102 ? ((const uint32_t*)ip)[dc.startIndex + k] : ((const uint16_t*)ip)[dc.startIndex + k];
					}
					else vi += k;
					const float* f = (const float*)((const uint8_t*)guest::host(s0.vb->data) + s0.offset + (uint64_t)vi * s0.stride);
					char b6[120]; snprintf(b6, sizeof b6, " v%lld(%g %g %g)", (long long)vi, (double)f[0], (double)f[1], (double)f[2]);
					o += b6;
				}
				log("DD3   verts%s startIdx %u base %d vp %u,%u,%u,%u z %g..%g", o.c_str(), dc.startIndex, (int)dc.baseVertex, dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3], (double)dev.vpZ[0], (double)dev.vpZ[1]);
			}
		}
		log("DD #%d prim %u cnt %u vs %p ps %p%s | z %u zw %u blend %u src %u dst %u cull %u at %u | rt %p", dn++, dc.prim, dc.primCount, (void*)vs, (void*)ps, tx.c_str(),
			dev.rs[7], dev.rs[14], dev.rs[27], dev.rs[19], dev.rs[20], dev.rs[22], dev.rs[15], (void*)dev.rt0);
	}
	bindTargets();
	uint32_t tw, th;
	targetSize(dev.rt0, tw, th);
	uint64_t t1 = nowUs();
	applyState(tw, th);
	glUseProgram(p->id);
	uint64_t t2 = nowUs();
	glBindVertexArray(gVao);
	// constants
	if (p->vc >= 0) glUniform4fv(p->vc, std::max(1, std::min(256, vs->info.numC + 0)), dev.vsF);
	if (p->vci >= 0) glUniform4iv(p->vci, 16, dev.vsI);
	if (p->vcb >= 0) glUniform1iv(p->vcb, 16, dev.vsB);
	if (p->pc >= 0) glUniform4fv(p->pc, std::max(1, std::min(224, ps->info.numC + 0)), dev.psF);
	if (p->pci >= 0) glUniform4iv(p->pci, 16, dev.psI);
	if (p->pcb >= 0) glUniform1iv(p->pcb, 16, dev.psB);
	if (p->flip >= 0) glUniform1f(p->flip, -1.f);
	if (p->at >= 0) glUniform4f(p->at, dev.rs[15] ? (float)dev.rs[25] : 0.f, (dev.rs[24] & 255) / 255.f, 0.f, 0.f);
	uint64_t t3 = nowUs();
	bindSamplers(p, ps);
	uint64_t t4 = nowUs();
	// vertex attributes
	std::vector<DeclElem> els;
	if (dev.vdecl)
	{
		size_t n = dev.vdecl->blob.size() / 8;
		for (size_t i = 0; i < n; i++)
		{
			DeclElem e;
			memcpy(&e, dev.vdecl->blob.data() + 8 * i, 8);
			if (e.stream == 0xff) break;
			els.push_back(e);
		}
	}
	else
	{
		static int ff = 0;
		if (ff++ < 3) log("d3d9: draw with FVF %x and no declaration - not supported yet", dev.fvf);
		return;
	}
	uint32_t enabled = 0, bgra = 0;
	GLuint boundBuf = 0;
	uint32_t upBytes = 0;
	if (dc.upVerts)
	{
		uint32_t nverts = dc.indexed ? dc.minIndex + dc.numVertices : (uint32_t)primVertices(dc.prim, dc.primCount);
		upBytes = nverts * dc.upStride;
		glBindBuffer(GL_ARRAY_BUFFER, gUpVbo);
		glBufferData(GL_ARRAY_BUFFER, upBytes, dc.upVerts, GL_STREAM_DRAW);
		boundBuf = gUpVbo;
	}
	const uint32_t nvAll = (uint32_t)primVertices(dc.prim, dc.primCount);
	// exact vertex range from the indices: the game passes numVertices for a whole buffer
	uint32_t useMin = dc.minIndex, useCount = dc.numVertices;
	if (dc.indexed && !dc.upIdx && dev.ib && dev.ib->data && (uint64_t)(dc.startIndex + nvAll) * (dev.ib->fmt == 102 ? 4 : 2) <= dev.ib->size && nvAll)
	{
		const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * (dev.ib->fmt == 102 ? 4 : 2);
		uint32_t lo = 0xffffffffu, hi = 0;
		if (dev.ib->fmt == 102) for (uint32_t i = 0; i < nvAll; i++) { uint32_t v; memcpy(&v, ip + 4 * i, 4); lo = std::min(lo, v); hi = std::max(hi, v); }
		else for (uint32_t i = 0; i < nvAll; i++) { uint16_t v; memcpy(&v, ip + 2 * i, 2); lo = std::min<uint32_t>(lo, v); hi = std::max<uint32_t>(hi, v); }
		useMin = lo; useCount = hi - lo + 1;
	}
	// hardware instancing (D3D9 SetStreamSourceFreq): stream with 0x40000000|n holds the geometry, streams with 0x80000000|1 hold per-instance data
	uint32_t instCount = 1;
	bool instanced = false;
	if (!dc.upVerts)
		for (int si = 0; si < 16; si++)
		{
			uint32_t fq = dev.streams[si].freq;
			if (fq & 0x40000000u) { instCount = std::max<uint32_t>(1, fq & 0x3fffffffu); instanced = true; }
			else if (fq & 0x80000000u) instanced = true;
		}
	static bool gInstUsed = false;
	if (instanced) { static int n = 0; if (n++ < 10) log("draw: instanced x%u prim %u count %u", instCount, dc.prim, dc.primCount); }
	int64_t ringBase[16];
	for (int i = 0; i < 16; i++) ringBase[i] = -2; // -2: not tried yet, -1: not on the ring
	for (auto& e : els)
	{
		int loc = shdr::attrLocation(e.usage, e.index);
		if (loc < 0) continue;
		GLint size;
		GLenum gtype;
		bool norm;
		if (!attribFormat(e.type, size, gtype, norm)) continue;
		Stream& st = dev.streams[e.stream < 16 ? e.stream : 0];
		uint32_t stride;
		uintptr_t off = e.offset;
		if (dc.upVerts)
		{
			if (e.stream != 0) continue;
			stride = dc.upStride;
		}
		else
		{
			if (!st.vb) continue;
			stride = st.stride;
			int si = e.stream < 16 ? e.stream : 0;
			if (instanced && (st.freq & 0x80000000u))
			{
				// per-instance stream: plain buffer, no base vertex, divisor 1
				glBindBuffer(GL_ARRAY_BUFFER, ensureBuffer(st.vb, GL_ARRAY_BUFFER));
				if (e.type == 4) bgra |= 1u << loc;
				glEnableVertexAttribArray(loc);
				glVertexAttribPointer(loc, size, gtype, norm, stride, (const void*)(uintptr_t)(st.offset + off));
				glVertexAttribDivisor(loc, 1);
				gInstUsed = true;
				enabled |= 1u << loc;
				continue;
			}
			if (ringBase[si] == -2)
			{
				ringBase[si] = -1;
				if ((st.vb->usage & 0x200) && st.vb->data && stride)
				{
					int64_t first = dc.indexed ? (int64_t)dc.baseVertex + useMin : dc.baseVertex;
					int64_t count = dc.indexed ? useCount : nvAll;
					int64_t avail = ((int64_t)st.vb->size - (int64_t)st.offset) / (int64_t)stride - first;
					if (count > avail) count = avail;
					if (first >= 0 && count > 0)
					{
						uint32_t head = dc.indexed ? useMin * stride : 0;
						int64_t at = ringPut(GL_ARRAY_BUFFER, gRingVbo, gRingVPos, kRingV, nullptr, head,
							(const uint8_t*)guest::host(st.vb->data) + st.offset + (uint64_t)first * stride, (uint32_t)(count * stride));
						if (at >= 0) ringBase[si] = at - head;
					}
				}
			}
			if (ringBase[si] >= 0)
			{
				glBindBuffer(GL_ARRAY_BUFFER, gRingVbo);
				off += (uint64_t)ringBase[si];
			}
			else
			{
				glBindBuffer(GL_ARRAY_BUFFER, ensureBuffer(st.vb, GL_ARRAY_BUFFER));
				off += st.offset + (int64_t)dc.baseVertex * (int64_t)stride;
			}
		}
		if (e.type == 4) bgra |= 1u << loc;
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, size, gtype, norm, stride, (const void*)off);
		enabled |= 1u << loc;
	}
	for (int loc = 0; loc < 16; loc++)
		if (!(enabled >> loc & 1)) { glDisableVertexAttribArray(loc); glVertexAttrib4f(loc, 0.f, 0.f, 0.f, 1.f); }
	if (p->bgra >= 0) glUniform1i(p->bgra, (GLint)bgra);
	uint64_t t5 = nowUs();
	GLenum mode = primMode(dc.prim);
	if (verbose()) { static uint64_t cnt[8]; static uint32_t shown; if (dc.prim < 8) cnt[dc.prim]++; if ((dc.prim == 1 || dc.prim == 2 || dc.prim == 3) && shown < 20) { shown++; log("draw: prim %u count %u indexed %d stride? rt %p", dc.prim, dc.primCount, (int)dc.indexed, (void*)dev.rt0); } if (gDrawsThisFrame == 0 && gFrameNo % 300 == 0) log("draw: prim totals pt %llu line %llu strip %llu tri %llu tstrip %llu tfan %llu", (unsigned long long)cnt[1], (unsigned long long)cnt[2], (unsigned long long)cnt[3], (unsigned long long)cnt[4], (unsigned long long)cnt[5], (unsigned long long)cnt[6]); }
	uint32_t nv = (uint32_t)primVertices(dc.prim, dc.primCount);
	if (!dc.indexed)
	{
		if (instanced) glDrawArraysInstanced(mode, 0, nv, (GLsizei)instCount); else glDrawArrays(mode, 0, nv);
	}
	else if (dc.upIdx)
	{
		bool wide = dc.upIdxFmt == 102;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gUpIbo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, nv * (wide ? 4 : 2), dc.upIdx, GL_STREAM_DRAW);
		glDrawElements(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
	}
	else if (dev.ib)
	{
		bool wide = dev.ib->fmt == 102;
		uint32_t isz = wide ? 4 : 2;
		int64_t iat = -1;
		if ((dev.ib->usage & 0x200) && dev.ib->data && (uint64_t)(dc.startIndex + nv) * isz <= dev.ib->size)
			iat = ringPut(GL_ELEMENT_ARRAY_BUFFER, gRingIbo, gRingIPos, kRingI, nullptr, 0,
				(const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * isz, nv * isz);
		if (iat >= 0)
		{
			if (instanced) glDrawElementsInstanced(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)iat, (GLsizei)instCount);
			else glDrawElements(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)iat);
		}
		else
		{
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ensureBuffer(dev.ib, GL_ELEMENT_ARRAY_BUFFER));
			if (instanced) glDrawElementsInstanced(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)(dc.startIndex * isz), (GLsizei)instCount);
			else glDrawElements(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)(dc.startIndex * isz));
		}
	}
	if (gInstUsed) { for (int loc = 0; loc < 16; loc++) glVertexAttribDivisor(loc, 0); gInstUsed = false; }
	uint64_t t6 = nowUs();
	tProg += t1 - t0; tState += t2 - t1; tUni += t3 - t2; tSamp += t4 - t3; tAttr += t5 - t4; tDraw += t6 - t5; nDraw++;
	gDrawsThisFrame++;
	if (gDrawsThisFrame <= 3 && gFrameNo < 5) glErr("draw");
	if ((gFrameNo == 700 || gFrameNo == 1800 || gFrameNo == 3600) && !dev.rt0)
	{
		uint32_t dn = gDrawsThisFrame;
		if (dn == 1 || dn == 10 || dn == 30 || dn == 60 || dn == 90 || dn == 120) dumpBmp(("/sdcard/ra3_f" + std::to_string(gFrameNo) + "_d" + std::to_string(dn) + ".bmp").c_str(), tw, th);
	}
}

static void reapReleased(uint32_t frame);
void present()
{
	{ static int n = 0; if (n++ < 5) log("GL: present on tid %d ctx %p", (int)gettid(), (void*)eglGetCurrentContext()); }
	if (!ensureGl()) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); return; }
	gfx::beginFrame();
	resizeBackBufferImpl();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	int sw = gfx::width(), sh = gfx::height();
	glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
	glColorMask(1, 1, 1, 1);
	glViewport(0, 0, sw, sh);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	// letterbox the back buffer
	double sa = (double)sw / sh, ba = (double)backW / backH;
	int vw = sw, vh = sh, vx = 0, vy = 0;
	if (sa > ba) { vw = (int)(sh * ba); vx = (sw - vw) / 2; } else { vh = (int)(sw / ba); vy = (sh - vh) / 2; }
	input::setView(vx, vy, vw, vh, (int)backW, (int)backH);
	glViewport(vx, vy, vw, vh);
	glUseProgram(gBlitProg);
	glBindVertexArray(gVao);
	for (int loc = 0; loc < 16; loc++) glDisableVertexAttribArray(loc);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, gBackTex);
	glUniform1i(glGetUniformLocation(gBlitProg, "t"), 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// the status/log overlay is only for the very start (nothing rendered yet); once the game has drawn anything, empty frames
	// (loading screens) must not flash it
	static bool everDrew = false;
	if (gDrawsThisFrame > 0) everDrew = true;
	if (gDrawsThisFrame == 0 && !everDrew) { glViewport(0, 0, sw, sh); gfx::drawOverlay(); }
	{
		// on-screen keyboard button: drawn with scissored clears (no shader needed)
		input::setScreen(sw, sh);
		int kx, ky, kw, kh;
		input::kbButtonRect(kx, ky, kw, kh);
		glViewport(0, 0, sw, sh);
		glEnable(GL_SCISSOR_TEST);
		auto rect = [&](int x, int y, int w, int h, float r, float g, float b) { glScissor(x, sh - y - h, w, h); glClearColor(r, g, b, 1.f); glClear(GL_COLOR_BUFFER_BIT); };
		rect(kx, ky, kw, kh, 0.75f, 0.75f, 0.78f);
		rect(kx + 3, ky + 3, kw - 6, kh - 6, 0.12f, 0.12f, 0.14f);
		int pad = kh / 8, rows = 3, cols = 7, cw = (kw - 6 - pad * 2) / cols, rh = (kh - 6 - pad * 2) / rows;
		for (int r = 0; r < rows; r++)
			for (int cc = 0; cc < cols; cc++)
				if (!(r == 2 && cc > 0 && cc < cols - 1) || cc == 3) rect(kx + 3 + pad + cc * cw + 1, ky + 3 + pad + r * rh + 1, (r == 2 && cc == 3 ? cw * 3 : cw) - 2, rh - 2, 0.85f, 0.85f, 0.88f);
		hud::draw(rect);
		glDisable(GL_SCISSOR_TEST);
	}
	gfx::swap();
	glErr("present");
	gFrameNo++;
	{
		// load-time log: the first frame and every pause between frames longer than 1.5 s, with the share spent reading from disk
		static uint64_t lastT = 0, lastBytes = 0, lastUs = 0, lastCalls = 0;
		uint64_t t = nowUs(), b, u, n; rt::ioStats(b, u, n);
		if (gFrameNo == 1)
			log("timing: first frame (see the timestamp); disk reads so far: %llu MiB, %llu calls, %.1f s", (unsigned long long)(b >> 20), (unsigned long long)n, u / 1e6);
		else if (lastT && t - lastT > 1500000)
			log("timing: pause between frames %.1f s; disk reads in it: %llu MiB, %llu calls, %.1f s", (t - lastT) / 1e6,
				(unsigned long long)((b - lastBytes) >> 20), (unsigned long long)(n - lastCalls), (u - lastUs) / 1e6);
		lastT = t; lastBytes = b; lastUs = u; lastCalls = n;
	}
	if (gFrameNo % 600 == 0) reapReleased(gFrameNo);
	if (gFrameNo % 300 == 0 && verbose())
	{
		static uint64_t last = 0; uint64_t n = nowUs();
		log("perf: ring %llu calls %llu KB %llu us, %llu orphans, %llu too big", (unsigned long long)gRingCalls, (unsigned long long)(gRingBytes >> 10), (unsigned long long)gRingUs, (unsigned long long)gRingOrph, (unsigned long long)gRingNs);
		gRingCalls = gRingBytes = gRingUs = gRingOrph = gRingNs = 0;
		log("perf: upload %llu KB, %llu full, %llu sub, %llu us, max buf %llu, dynamic %llu", (unsigned long long)(gUpBytes >> 10), (unsigned long long)gUpFull, (unsigned long long)gUpSub, (unsigned long long)gUpUs, (unsigned long long)gUpMaxSize, (unsigned long long)gUpDyn);
		gUpUs = gUpMaxSize = gUpDyn = 0;
		gUpBytes = gUpFull = gUpSub = 0;
		log("perf: 300 frames in %llu ms; per draw us: prog %llu state %llu uni %llu samp %llu attr %llu draw %llu (n %llu)", (unsigned long long)((n - last) / 1000), (unsigned long long)(tProg / std::max<uint64_t>(1, nDraw)), (unsigned long long)(tState / std::max<uint64_t>(1, nDraw)), (unsigned long long)(tUni / std::max<uint64_t>(1, nDraw)), (unsigned long long)(tSamp / std::max<uint64_t>(1, nDraw)), (unsigned long long)(tAttr / std::max<uint64_t>(1, nDraw)), (unsigned long long)(tDraw / std::max<uint64_t>(1, nDraw)), (unsigned long long)nDraw);
		last = n; tProg = tState = tUni = tSamp = tAttr = tDraw = nDraw = 0;
	}
	if (gFrameNo == 200)
	{
		// experiment: /sdcard/ra3_lod containing a digit 0..3 forces the static LOD level (and the detected 'ideal' level)
		if (FILE* f = fopen("/sdcard/ra3_lod", "r"))
		{
			int d = -1; if (fscanf(f, "%d", &d) != 1) d = -1; fclose(f);
			uint32_t gm = guest::load<uint32_t>(0xce2f98);
			if (gm && d >= 0 && d <= 5)
			{
				log("LOD: forcing static level %d (was %d, ideal %d)", d, (int)guest::load<uint32_t>(gm + 0x2dc), (int)guest::load<uint32_t>(gm + 0x340));
				// apply the preset through the game's own function (0x61d660 = GameLODManager::applyStaticLOD(level)), preserving the guest registers
				if (CPU* cc = rt::thisThread().cpu)
				{
					uint32_t sv[8]; memcpy(sv, cc->r, sizeof sv);
					cc->r[ECX] = gm;
					rt::callGuest(cc, 0x61d660, {(uint32_t)d});
					memcpy(cc->r, sv, sizeof sv);
					log("LOD: applied preset %d via 0x61d660", d);
				}
				guest::store<uint32_t>(gm + 0x2dc, (uint32_t)d);
				guest::store<uint32_t>(gm + 0x340, (uint32_t)d);
			}
		}
	}
	if (gFrameNo == 900 || gFrameNo == 1800 || gFrameNo == 3600 || gFrameNo == 5400)
	{
		uint32_t gm = guest::load<uint32_t>(0xce2f98);
		log("LOD: manager %08x; hw: cpu %u/%u/%u mem %08x%08x vid %u %u", gm, guest::load<uint32_t>(0xcf00b8), guest::load<uint32_t>(0xcf046c), guest::load<uint32_t>(0xcefa60), guest::load<uint32_t>(0xcee5d4), guest::load<uint32_t>(0xcee5d0), guest::load<uint32_t>(0xcf0414), guest::load<uint32_t>(0xce2fa8));
		log("LOD: cpulib flag %02x tscfreq %08x%08x cf009c %x cee3a8 %x cee3ac %x", guest::load<uint8_t>(0xcf0423), guest::load<uint32_t>(0xcf0094), guest::load<uint32_t>(0xcf0090), guest::load<uint32_t>(0xcf009c), guest::load<uint32_t>(0xcee3a8), guest::load<uint32_t>(0xcee3ac));
		if (gm && gFrameNo == 1800)
		{
			auto str = [](uint32_t a) -> std::string { if (a < 0x400000 || a >= 0x30000000) return ""; std::string r; for (int i = 0; i < 40; i++) { char ch = (char)guest::load<uint8_t>(a + i); if (!ch) break; if (ch < 32 || ch > 126) return ""; r += ch; } return r.size() >= 3 ? r : ""; };
			std::string raw; char b[16];
			for (uint32_t o = 0; o < 0x2d8; o += 4) { snprintf(b, sizeof b, "%x:%x ", o, guest::load<uint32_t>(gm + o)); raw += b; }
			log("LODRAW: %s", raw.c_str());
			auto dumpObj = [&](const char* tag, uint32_t p) {
				if (p < 0x1000000 || p >= 0x30000000) return;
				std::string l; for (uint32_t o = 0; o < 0x40; o += 4) { uint32_t v = guest::load<uint32_t>(p + o); snprintf(b, sizeof b, "%x ", v); l += b; std::string t = str(v); if (!t.empty()) l += "'" + t + "' "; }
				log("LODOBJ %s %08x: %s", tag, p, l.c_str());
			};
			for (uint32_t i = 0; i < 4; i++) dumpObj(("a24[" + std::to_string(i) + "]").c_str(), guest::load<uint32_t>(gm + 0x24 + i * 4));
			for (uint32_t i = 0; i < 6; i++) dumpObj(("a3c[" + std::to_string(i) + "]").c_str(), guest::load<uint32_t>(gm + 0x3c + i * 4));
			for (uint32_t i = 0; i < 2; i++) dumpObj(("a54[" + std::to_string(i) + "]").c_str(), guest::load<uint32_t>(gm + 0x54 + i * 4));
			for (uint32_t i = 0; i < 5; i++) { uint32_t n = guest::load<uint32_t>(gm + 0x32c + i * 4); log("LODLIST idx %u count %u", i, n); for (uint32_t k = 1; k <= n && k <= 8; k++) dumpObj(("list" + std::to_string(i) + "." + std::to_string(k)).c_str(), guest::load<uint32_t>(gm + 0x58 + ((i * 32 + k) * 4))); }
			for (uint32_t i = 0; i < 4; i++) dumpObj(("a5c[" + std::to_string(i) + "]").c_str(), guest::load<uint32_t>(gm + 0x5c + i * 128));
		}
		if (gm) { std::string line; char b[16]; for (uint32_t o = 0x2d8; o < 0x364; o += 4) { snprintf(b, sizeof b, "%x:%x ", o, guest::load<uint32_t>(gm + o)); line += b; } log("LOD: %s", line.c_str()); }
	}
	if (gFrameNo % 300 == 0) rt::profDump();
	if (gFrameNo == 1800 || gFrameNo == 3600) { log("DIAG: call rings of all threads at frame %u", gFrameNo); rt::traceDump(); }
	if (gFrameNo == 1 || gFrameNo % 3600 == 0) log("d3d9: frame %u presented, %u draws in the last frame, screen %dx%d", gFrameNo, gDrawsThisFrame.load(), sw, sh);
	gDrawsThisFrame = 0;
	gfx::release();   // hand the context over: a different game thread may render next
}

// Reclaim objects the game released: GL names and the guest-side copy of the data.
static void reapReleased(uint32_t frame)
{
	static uint64_t freedBytes = 0, freedObjs = 0;
	static uint32_t lastLog = 0;
	for (Obj* o : takeReleased())
	{
		if (o->ref > 0) { o->released = false; continue; }                       // referenced again
		bool bound = o == dev.ib || o == dev.rt0 || o == dev.ds || o == gBackBuffer;
		for (int i = 0; i < 16 && !bound; i++) bound = dev.tex[i] == o || dev.streams[i].vb == o;
		if (bound || frame - o->lastUse < 1800 || o->lastUse > frame) { requeueReleased(o); continue; }
		if (o->glTex) { glDeleteTextures(1, &o->glTex); o->glTex = 0; }
		if (o->glBuf) { glDeleteBuffers(1, &o->glBuf); o->glBuf = 0; }
		o->glVer = 0; o->glSize = 0;
		if (o->ownsData && o->data) { freedBytes += o->size; rt::gfree(o->data); o->data = 0; }
		freedObjs++;
		o->released = false;   // a later AddRef/Release cycle queues it again
	}
	if (frame - lastLog >= 3600)
	{
		lastLog = frame;
		log("mem: guest heap %llu MiB live; reclaimed %llu d3d objects, %llu MiB so far", (unsigned long long)(rt::heapLiveBytes() >> 20), (unsigned long long)freedObjs, (unsigned long long)(freedBytes >> 20));
	}
}

void stretchRect(Obj* src, const int32_t* sr, Obj* dst, const int32_t* dr, uint32_t filter)
{
	if (!ensureGl()) return;
	uint32_t sw, sh, dw, dh;
	targetSize(src, sw, sh);
	targetSize(dst, dw, dh);
	// read side
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gReadFbo);
	if (isBack(src)) glBindFramebuffer(GL_READ_FRAMEBUFFER, gBackFbo);
	else
	{
		GLuint t = ensureTexture(src);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, src->iface == ISurface ? src->level : 0);
	}
	if (isBack(dst)) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gBackFbo);
	else
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gDrawFbo);
		GLuint t = ensureTexture(dst);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, dst->iface == ISurface ? dst->level : 0);
	}
	glDisable(GL_SCISSOR_TEST);
	int32_t s[4] = {0, 0, (int32_t)sw, (int32_t)sh}, d[4] = {0, 0, (int32_t)dw, (int32_t)dh};
	if (sr) memcpy(s, sr, 16);
	if (dr) memcpy(d, dr, 16);
	glBlitFramebuffer(s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], GL_COLOR_BUFFER_BIT, filter == 2 ? GL_LINEAR : GL_NEAREST);
	glErr("stretchRect");
}

void destroy(Obj*) {}
}   // namespace glr
}   // namespace d3d

#else   // not Android: no renderer
namespace d3d
{
namespace glr
{
bool init() { return false; }
bool ready() { return false; }
void clear(uint32_t, uint32_t, uint32_t, uint32_t, float, uint32_t) {}
void draw(const DrawCall&) {}
void present() {}
void resizeBackBuffer() {}
void stretchRect(Obj*, const int32_t*, Obj*, const int32_t*, uint32_t) {}
void destroy(Obj*) {}
}
}
#endif
