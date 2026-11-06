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
	if (!o->glTex)
	{
		glGenTextures(1, &o->glTex);
		o->glVer = 0;
		if (o->rtTex && (o->iface == ITexture || o->iface == ISurface)) { createRtStorage(o); o->glVer = o->ver; }
	}
	if (!o->rtTex && o->glVer != o->ver && o->data) uploadTexture(o);
	return o->glTex;
}

GLuint ensureBuffer(Obj* o, GLenum target)
{
	if (!o->glBuf) { glGenBuffers(1, &o->glBuf); o->glVer = 0; }
	if (o->glVer != o->ver)
	{
		glBindBuffer(target, o->glBuf);
		glBufferData(target, o->size, guest::host(o->data), GL_DYNAMIC_DRAW);
		o->glVer = o->ver;
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

void bindTargets()
{
	Obj* rt = dev.rt0;
	if (isBack(rt))
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo);
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, gDrawFbo);
	GLuint tex = ensureTexture(rt);
	uint32_t lvl = rt->iface == ISurface ? rt->level : 0;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, lvl);
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

bool ready() { return gOk; }

void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
	if (!gOk) return;
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

void draw(const DrawCall& dc)
{
	{ static int n = 0; if (n++ < 5) log("GL: draw on tid %d ctx %p", (int)gettid(), (void*)eglGetCurrentContext()); }
	if (!gOk) return;
	ShaderObj *vs = dev.vs, *ps = dev.ps;
	static int noShader = 0;
	if (!vs || !ps)
	{
		if (noShader++ < 5) log("d3d9: draw without shaders (fixed function not implemented): fvf %x vdecl %p", dev.fvf, (void*)dev.vdecl);
		return;
	}
	Prog* p = getProgram(vs, ps);
	if (!p) return;
	bindTargets();
	uint32_t tw, th;
	targetSize(dev.rt0, tw, th);
	applyState(tw, th);
	glUseProgram(p->id);
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
	bindSamplers(p, ps);
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
			glBindBuffer(GL_ARRAY_BUFFER, ensureBuffer(st.vb, GL_ARRAY_BUFFER));
			stride = st.stride;
			off += st.offset + (int64_t)dc.baseVertex * (int64_t)stride * (dc.indexed ? 1 : 0);
		}
		if (e.type == 4) bgra |= 1u << loc;
		glEnableVertexAttribArray(loc);
		glVertexAttribPointer(loc, size, gtype, norm, stride, (const void*)off);
		enabled |= 1u << loc;
	}
	for (int loc = 0; loc < 16; loc++)
		if (!(enabled >> loc & 1)) { glDisableVertexAttribArray(loc); glVertexAttrib4f(loc, 0.f, 0.f, 0.f, 1.f); }
	if (p->bgra >= 0) glUniform1i(p->bgra, (GLint)bgra);
	GLenum mode = primMode(dc.prim);
	uint32_t nv = (uint32_t)primVertices(dc.prim, dc.primCount);
	if (!dc.indexed)
	{
		glDrawArrays(mode, dc.upVerts ? 0 : dc.baseVertex, nv);
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
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ensureBuffer(dev.ib, GL_ELEMENT_ARRAY_BUFFER));
		glDrawElements(mode, nv, wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void*)(uintptr_t)(dc.startIndex * (wide ? 4 : 2)));
	}
	gDrawsThisFrame++;
	if (gDrawsThisFrame <= 3 && gFrameNo < 5) glErr("draw");
}

void present()
{
	{ static int n = 0; if (n++ < 5) log("GL: present on tid %d ctx %p", (int)gettid(), (void*)eglGetCurrentContext()); }
	if (!gOk) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); return; }
	gfx::beginFrame();
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
	glViewport(vx, vy, vw, vh);
	glUseProgram(gBlitProg);
	glBindVertexArray(gVao);
	for (int loc = 0; loc < 16; loc++) glDisableVertexAttribArray(loc);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, gBackTex);
	glUniform1i(glGetUniformLocation(gBlitProg, "t"), 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	if (gDrawsThisFrame == 0) { glViewport(0, 0, sw, sh); gfx::drawOverlay(); }
	gfx::swap();
	glErr("present");
	gFrameNo++;
	if (gFrameNo == 1 || gFrameNo % 300 == 0) log("d3d9: frame %u presented, %u draws in the last frame, screen %dx%d", gFrameNo, gDrawsThisFrame.load(), sw, sh);
	gDrawsThisFrame = 0;
}

void stretchRect(Obj* src, const int32_t* sr, Obj* dst, const int32_t* dr, uint32_t filter)
{
	if (!gOk) return;
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
void stretchRect(Obj*, const int32_t*, Obj*, const int32_t*, uint32_t) {}
void destroy(Obj*) {}
}
}
#endif
