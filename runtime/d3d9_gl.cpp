#include <set>
#include <string>
#include <cmath>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
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
#include "glrec.h"

using namespace rt;

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

void profStart();

namespace d3d
{
namespace glr
{
namespace
{
bool gInit = false, gOk = false;
bool gAttrDirty = true;   // vertex attribute state of gVao changed outside setAttrib (present disables all arrays)
bool gFboDirty = true;   // framebuffer bindings/attachments changed outside bindTargets
bool gTexBindDirty = true;   // a texture was bound outside bindSamplers: its binding cache is stale
GLuint gVao = 0, gBackFbo = 0, gBackTex = 0, gBackDepth = 0, gDrawFbo = 0, gReadFbo = 0, gUpVbo = 0, gUpIbo = 0;
GLuint gBlitProg = 0;
GLuint gSamplers[16] = {};
GLuint gDummy2D = 0, gDummyCube = 0, gDummy3D = 0;
bool gHasS3tc = false;
std::atomic<uint32_t> gDrawsThisFrame{0};
uint32_t gFrameNo = 0;
static inline uint64_t nowUs();
static inline uint64_t profNow() { static const bool v = verbose(); return v ? nowUs() : 0; }
static inline uint64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// frame-time breakdown (logged once a minute)
static uint64_t gTDraw = 0, gTUpload = 0, gTPresent = 0, gNDraws = 0, gNUploads = 0, gTWaitRender = 0;
static std::atomic<uint64_t> gTSwap{0};
int gErrLogs = 0;

void glErr(const char* where)
{
	if (rec::active()) { rec::q([where] { glErr(where); }); return; }   // checked on the render thread, in order
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

// ---- software DXT1/3/5 decoder for GPUs without GL_EXT_texture_compression_s3tc (Mali, PowerVR)
static void dxtColors(const uint8_t* b, uint8_t c[4][4], bool dxt1)
{
	uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
	auto rgb = [](uint16_t v, uint8_t* d) { d[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); d[2] = (uint8_t)((v & 31) * 255 / 31); d[3] = 255; };
	rgb(c0, c[0]); rgb(c1, c[1]);
	if (!dxt1 || c0 > c1)
		for (int k = 0; k < 3; k++) { c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3); c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3); }
	else
		for (int k = 0; k < 3; k++) { c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2); c[3][k] = 0; }
	c[2][3] = 255; c[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
}
static void decodeDxt(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	out.assign((size_t)w * h * 4, 0);
	bool dxt1 = fmt == 0x31545844, dxt3 = fmt == 0x33545844;
	uint32_t bwk = (w + 3) / 4, bhk = (h + 3) / 4, bs = dxt1 ? 8 : 16;
	for (uint32_t by = 0; by < bhk; by++)
		for (uint32_t bx = 0; bx < bwk; bx++)
		{
			const uint8_t* b = src + ((size_t)by * bwk + bx) * bs;
			uint8_t a[16];
			for (int i = 0; i < 16; i++) a[i] = 255;
			if (dxt3) for (int i = 0; i < 16; i++) a[i] = (uint8_t)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
			else if (!dxt1)
			{
				uint8_t a0 = b[0], a1 = b[1], t[8] = {a0, a1};
				if (a0 > a1) for (int k = 1; k < 7; k++) t[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1) / 7);
				else { for (int k = 1; k < 5; k++) t[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1) / 5); t[6] = 0; t[7] = 255; }
				uint64_t bits = 0; for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
				for (int i = 0; i < 16; i++) a[i] = t[(bits >> (3 * i)) & 7];
			}
			const uint8_t* cb = dxt1 ? b : b + 8;
			uint8_t c[4][4];
			dxtColors(cb, c, dxt1);
			uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
			for (int i = 0; i < 16; i++)
			{
				uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
				if (x >= w || y >= h) continue;
				const uint8_t* cc = c[(idx >> (2 * i)) & 3];
				uint8_t* d = &out[((size_t)y * w + x) * 4];
				d[0] = cc[0]; d[1] = cc[1]; d[2] = cc[2]; d[3] = dxt1 ? cc[3] : a[i];
			}
		}
}

// expands a level of a "convert" format to RGBA8
void expandLevel(uint32_t fmt, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out)
{
	if (fmt == 0x31545844 || fmt == 0x33545844 || fmt == 0x35545844) { decodeDxt(fmt, src, w, h, out); return; }
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

static void uploadTextureImpl(Obj* o);
void uploadTexture(Obj* o)
{
	uint64_t t0 = nowUs();
	uploadTextureImpl(o);
	gTUpload += nowUs() - t0; gNUploads++;
}
static void uploadTextureImpl(Obj* o)
{
	TexFmt tf = texFmtOf(o->fmt);
	GLenum target = targetOf(o);
	glBindTexture(target, o->glTex); gTexBindDirty = true;
	// GPUs without S3TC (Mali, PowerVR): DXT is decoded on the CPU. Opaque DXT1 becomes RGB565 (half the memory of RGBA8),
	// and textures of 512 px and up start at their second mip level (a quarter of the memory) unless /sdcard/ra3_fulltex exists
	bool soft = tf.compressed && !gHasS3tc;
	bool to565 = false;
	uint32_t skip = 0;
	if (soft)
	{
		tf.compressed = false; tf.convert = true;
		static const bool fullTex = access("/sdcard/ra3_fulltex", F_OK) == 0;
		if (!fullTex && o->levels > 1 && o->w >= 512 && o->h >= 512 && o->iface != IVolume) skip = 1;
		if (o->fmt == 0x31545844 && o->data)
		{
			// punch-through alpha: a block with c0 <= c1 that uses index 3
			to565 = true;
			for (uint32_t f = 0; f < o->faces && to565; f++)
				for (uint32_t l = skip; l < o->levels && to565; l++)
				{
					uint32_t w = std::max(1u, o->w >> l), h = std::max(1u, o->h >> l), nb = ((w + 3) / 4) * ((h + 3) / 4);
					const uint8_t* b = (const uint8_t*)guest::host(o->data + o->levelOff[f * o->levels + l]);
					for (uint32_t k = 0; k < nb; k++, b += 8)
					{
						uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
						if (c0 > c1) continue;
						uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
						bool t = false;
						for (int i = 0; i < 16; i++) if (((idx >> (2 * i)) & 3) == 3) { t = true; break; }
						if (t) { to565 = false; break; }
					}
				}
		}
		static int nl = 0;
		if (nl++ < 8) log("GL: no S3TC: decoding %ux%u DXT%c texture on the CPU (%s, from level %u)", o->w, o->h, (char)(o->fmt >> 24), to565 ? "RGB565" : "RGBA8", skip);
	}
	if (!(o->rtTex && !o->data))
	{
		std::vector<uint8_t> tmp;
		for (uint32_t f = 0; f < o->faces; f++)
			for (uint32_t l = skip; l < o->levels; l++)
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
						if (to565)
						{
							size_t n = (size_t)w * h;
							uint16_t* d16 = (uint16_t*)tmp.data();   // in place: each 4-byte pixel becomes 2 bytes
							for (size_t i = 0; i < n; i++) { const uint8_t* q = &tmp[i * 4]; d16[i] = (uint16_t)(((q[0] >> 3) << 11) | ((q[1] >> 2) << 5) | (q[2] >> 3)); }
							glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
							glTexImage2D(tgt, l, GL_RGB565, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tmp.data());
						}
						else glTexImage2D(tgt, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
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
	glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, (GLint)skip);
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
	glBindTexture(GL_TEXTURE_2D, o->glTex); gTexBindDirty = true;
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
			glBindTexture(GL_TEXTURE_CUBE_MAP, o->glTex); gTexBindDirty = true;
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
	glBindTexture(GL_TEXTURE_2D, gBackTex); gTexBindDirty = true;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, backW, backH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev); gTexBindDirty = true;
	glBindRenderbuffer(GL_RENDERBUFFER, gBackDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, backW, backH);
	glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo); gFboDirty = true;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gBackTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gBackDepth);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (gBackBuffer) { gBackBuffer->w = backW; gBackBuffer->h = backH; }
	// the device's own depth buffer is a texture of the old size: recreate it, otherwise every render target used with it
	// is clipped to the old resolution (world drawn in a corner after changing the resolution in the game's options)
	if (gDefaultDs && gDefaultDs->glTex) { glDeleteTextures(1, &gDefaultDs->glTex); gDefaultDs->glTex = 0; gDefaultDs->glVer = 0; gTexBindDirty = true; }
	log("GL: back buffer resized to %ux%u (status 0x%x)", backW, backH, glCheckFramebufferStatus(GL_FRAMEBUFFER));
}

void bindTargets()
{
	resizeBackBufferImpl();
	Obj* rt = dev.rt0;
	static Obj* lastRt = nullptr; static Obj* lastDs = nullptr; static GLuint lastTex = 0, lastDt = 0; static uint32_t lastLvl = ~0u;
	if (isBack(rt))
	{
		if (gFboDirty || lastRt != rt) { glBindFramebuffer(GL_FRAMEBUFFER, gBackFbo); lastRt = rt; gFboDirty = false; }
		return;
	}
	GLuint tex = ensureTexture(rt);
	{
		// D3DUSAGE_AUTOGENMIPMAP render target (the 128x128 environment cube): the lower levels must be rebuilt before it is
		// sampled, otherwise they hold whatever the GPU memory contained (noisy blue reflections after loading a save)
		Obj* ow = texOwner(rt);
		if (ow->levels > 1 && (ow->usage & 0x400) && (rt->iface != ISurface || rt->level == 0)) ow->mipDirty = true;
	}
	{
		// same colour/depth targets as the previous draw: attachments are still in place
		Obj* ds0 = dev.ds && dev.ds->fmt != 0x4c4c554e ? dev.ds : nullptr;
		GLuint dt0 = ds0 ? ensureTexture(ds0) : 0;
		uint32_t lvl0 = rt->iface == ISurface ? rt->level : 0;
		if (!gFboDirty && lastRt == rt && lastDs == ds0 && lastTex == tex && lastDt == dt0 && lastLvl == lvl0) return;
		lastRt = rt; lastDs = ds0; lastTex = tex; lastDt = dt0; lastLvl = lvl0; gFboDirty = false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, gDrawFbo);
	uint32_t lvl = rt->iface == ISurface ? rt->level : 0;
	Obj* owner = texOwner(rt);
	GLenum faceTarget = owner->iface == ICube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (rt->iface == ISurface ? rt->face : 0) : GL_TEXTURE_2D;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, faceTarget, tex, lvl);
	Obj* ds = dev.ds;
	if (ds && ds->fmt != 0x4c4c554e)
	{
		{
			// diagnostics: a depth buffer smaller than the colour target limits GL rendering to their overlap
			uint32_t rw, rh, dw, dh;
			targetSize(rt, rw, rh); targetSize(ds, dw, dh);
			static int nm = 0;
			if ((dw < rw || dh < rh) && nm < 12) { nm++; log("GL: depth buffer %ux%u (fmt %u) smaller than render target %ux%u (fmt %u), frame %u", dw, dh, ds->fmt, rw, rh, rt->fmt, gFrameNo); }
		}
		TexFmt tf = texFmtOf(ds->fmt);
		GLuint dt = ensureTexture(ds);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, tf.depth ? dt : 0, 0);
	}
	else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	static int logged = 0, checks = 0;
	if (logged < 6 && checks < 200)
	{
		// the status query is synchronous with the render thread: only check the first binds
		checks++;
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
	GLint vc = -1, vci = -1, vcb = -1, pc = -1, pci = -1, pcb = -1, flip = -1, bgra = -1, at = -1, hp = -1, ui = -1, s[16];
	// last values uploaded to this program (uniforms persist per program): only changed ranges are re-sent
	bool samplersSet = false, shadowValid = false;
	std::vector<float> vcShadow, pcShadow;
	std::vector<GLint> vcLoc, pcLoc;   // location of each array element
	int32_t vciShadow[64], psiShadow[64], vcbShadow[16], pcbShadow[16];
	float flipShadow = 0.f, atShadow[4] = {-1, -1, -1, -1}, hpShadow[2] = {-1, -1}, uiShadow[3] = {0, 0, 0};
	GLint bgraShadow = -1;
};
// Uploads registers [0, n) of `src` to a vec4 uniform array, sending only the range that differs from `shadow`.
static void uploadVec4Range(GLint base, std::vector<GLint>& locs, std::vector<float>& shadow, const float* src, int n, GLuint prog, const char* name, bool valid)
{
	if (base < 0 || n <= 0) return;
	if ((int)shadow.size() < n * 4) { shadow.assign(n * 4, 0.f); valid = false; }
	int lo = 0, hi = n - 1;
	if (valid)
	{
		while (lo < n && memcmp(&shadow[lo * 4], src + lo * 4, 16) == 0) lo++;
		if (lo == n) return;
		while (hi > lo && memcmp(&shadow[hi * 4], src + hi * 4, 16) == 0) hi--;
	}
	if (lo > 0)
	{
		if (locs.empty())
		{
			locs.assign(n, -1);
			for (int i = 0; i < n; i++) { char b[32]; snprintf(b, sizeof b, "%s[%d]", name, i); locs[i] = glGetUniformLocation(prog, b); }
		}
		if (lo >= (int)locs.size() || locs[lo] < 0) lo = 0;
	}
	glUniform4fv(lo == 0 ? base : locs[lo], hi - lo + 1, src + lo * 4);
	memcpy(&shadow[lo * 4], src + lo * 4, (size_t)(hi - lo + 1) * 16);
}
std::unordered_map<uint64_t, Prog> gProgs;

// ---- program binary cache: linked programs are saved to <app data>/shadercache, so later runs skip compiling.
// Keyed by the GLSL sources and the driver; a binary the driver rejects is simply rebuilt and overwritten.
static std::string gBinDir;
static uint64_t gDriverKey = 0;
static uint64_t fnv64(const void* d, size_t n, uint64_t h = 1469598103934665603ull)
{
	const uint8_t* p = (const uint8_t*)d;
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}
static uint64_t binKey(const std::string& v, const std::string& f)
{
	return fnv64(f.data(), f.size(), fnv64(v.data(), v.size(), gDriverKey ^ 0x52413350u));
}
static void initBinCache()
{
	const char* r = (const char*)glGetString(GL_RENDERER);
	const char* ver = (const char*)glGetString(GL_VERSION);
	gDriverKey = fnv64(r ? r : "", r ? strlen(r) : 0, fnv64(ver ? ver : "", ver ? strlen(ver) : 0));
	gBinDir = rt::config.dataRoot + "/shadercache";
	mkdir(gBinDir.c_str(), 0755);
	// keep it bounded: past 96 MB start over
	uint64_t total = 0; unsigned files = 0;
	if (DIR* d = opendir(gBinDir.c_str()))
	{
		while (dirent* e = readdir(d))
		{
			if (e->d_name[0] == '.') continue;
			struct stat st;
			if (stat((gBinDir + "/" + e->d_name).c_str(), &st) == 0) { total += st.st_size; files++; }
		}
		closedir(d);
	}
	if (total > (96ull << 20))
	{
		if (DIR* d = opendir(gBinDir.c_str())) { while (dirent* e = readdir(d)) if (e->d_name[0] != '.') unlink((gBinDir + "/" + e->d_name).c_str()); closedir(d); }
		log("GL: shader cache was %llu MB, cleared", (unsigned long long)(total >> 20));
		total = 0; files = 0;
	}
	log("GL: shader cache %s: %u programs, %llu KB", gBinDir.c_str(), files, (unsigned long long)(total >> 10));
}
static std::string binPath(uint64_t key) { char b[32]; snprintf(b, sizeof b, "/%016llx.bin", (unsigned long long)key); return gBinDir + b; }
static bool loadProgramBinary(uint64_t key, Prog& p)
{
	if (gBinDir.empty()) return false;
	FILE* f = fopen(binPath(key).c_str(), "rb");
	if (!f) return false;
	uint32_t hdr[3] = {};
	std::vector<uint8_t> data;
	bool okRead = fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x50334152u && hdr[2] > 0 && hdr[2] < (64u << 20);
	if (okRead) { data.resize(hdr[2]); okRead = fread(data.data(), 1, data.size(), f) == data.size(); }
	fclose(f);
	if (!okRead) return false;
	p.id = glCreateProgram();
	glProgramBinary(p.id, (GLenum)hdr[1], data.data(), (GLsizei)data.size());
	GLint ok = 0;
	glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
	if (!ok) { unlink(binPath(key).c_str()); p.id = 0; return false; }   // driver changed: rebuild
	return true;
}
static void saveProgramBinary(uint64_t key, GLuint prog)
{
	if (gBinDir.empty()) return;
	GLint len = 0;
	glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
	if (len <= 0 || len > (64 << 20)) return;
	std::vector<uint8_t> data(len);
	GLenum fmt = 0; GLsizei got = 0;
	glGetProgramBinary(prog, len, &got, &fmt, data.data());
	if (got <= 0) return;
	std::string path = binPath(key), tmp = path + ".tmp";
	if (FILE* f = fopen(tmp.c_str(), "wb"))
	{
		uint32_t hdr[3] = {0x50334152u, (uint32_t)fmt, (uint32_t)got};
		bool w = fwrite(hdr, 4, 3, f) == 3 && fwrite(data.data(), 1, got, f) == (size_t)got;
		fclose(f);
		if (w) rename(tmp.c_str(), path.c_str()); else unlink(tmp.c_str());
	}
}
static Prog* finishProgram(Prog& p, ShaderObj* vs, ShaderObj* ps, bool cached);

Prog* getProgram(ShaderObj* vs, ShaderObj* ps)
{
	uint64_t key = vs->hash * 1000003ull ^ (ps->hash + 0x9e3779b97f4a7c15ull + (vs->hash << 6));
	auto it = gProgs.find(key);
	if (it != gProgs.end()) return it->second.failed ? nullptr : &it->second;
	Prog& p = gProgs[key];
	p.failed = true;
	if (!vs->ok || !ps->ok || !vs->info.isVs || ps->info.isVs) return nullptr;
	std::string vsrc = shdr::compose(vs->info), psrc = shdr::compose(ps->info, &vs->info.outputs);
	uint64_t bkey = binKey(vsrc, psrc);
	if (loadProgramBinary(bkey, p)) return finishProgram(p, vs, ps, true);
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
	glProgramParameteri(p.id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
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
	saveProgramBinary(bkey, p.id);
	return finishProgram(p, vs, ps, false);
}
static Prog* finishProgram(Prog& p, ShaderObj* vs, ShaderObj* ps, bool cached)
{
	p.failed = false;
	p.vc = glGetUniformLocation(p.id, "vc"); p.vci = glGetUniformLocation(p.id, "vci"); p.vcb = glGetUniformLocation(p.id, "vcb");
	p.pc = glGetUniformLocation(p.id, "pc"); p.pci = glGetUniformLocation(p.id, "pci"); p.pcb = glGetUniformLocation(p.id, "pcb");
	p.flip = glGetUniformLocation(p.id, "u_flip"); p.bgra = glGetUniformLocation(p.id, "u_bgra"); p.at = glGetUniformLocation(p.id, "u_at"); p.hp = glGetUniformLocation(p.id, "u_hp"); p.ui = glGetUniformLocation(p.id, "u_ui");
	for (int i = 0; i < 16; i++) { char n[8]; snprintf(n, sizeof n, "s%d", i); p.s[i] = glGetUniformLocation(p.id, n); }
	static int linked = 0;
	static int nCached = 0, nBuilt = 0;
	(cached ? nCached : nBuilt)++;
	if (linked++ < 8 || (nCached + nBuilt) % 100 == 0)
		log("GL: program #%d (vs %016llx ps %016llx) %s; so far %d from the shader cache, %d compiled", linked, (unsigned long long)vs->hash, (unsigned long long)ps->hash, cached ? "from cache" : "compiled", nCached, nBuilt);
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
static bool gHasBorder = false;
static float gMaxAniso = 1.f;   // GL_EXT_texture_filter_anisotropic   // GL_EXT/OES_texture_border_clamp (or GLES 3.2)
GLenum wrapMode(uint32_t a)
{
	// D3DTADDRESS_BORDER: samples outside [0,1] return the border colour (shadow maps rely on it: outside the map = lit)
	switch (a) { case 2: case 5: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE; case 4: return gHasBorder ? 0x812D : GL_CLAMP_TO_EDGE; default: return GL_REPEAT; }
}

// Everything applyState reads; when it equals the last applied key and nothing else touched GL state since
// (invalidateGlState: clears, copies, present), the ~20 GL state calls are skipped.
static uint32_t gStKey[44];
static bool gStValid = false;
static GLuint gCurProg = 0; static bool gVaoBound = false;
void invalidateGlState() { gStValid = false; gCurProg = 0; gVaoBound = false; gTexBindDirty = true; gFboDirty = true; gAttrDirty = true; }
void applyState(uint32_t tw, uint32_t th)
{
	const uint32_t* rs = dev.rs;
	bool hasDepth = isBack(dev.rt0) || (dev.ds && dev.ds->fmt != 0x4c4c554e);
	{
		static const uint8_t idx[] = {7, 14, 15, 19, 20, 22, 23, 27, 52, 53, 54, 55, 56, 57, 58, 59, 168, 171, 174, 175, 193, 195, 206, 207, 208, 209};
		uint32_t key[44]; uint32_t n = 0;
		for (uint8_t i : idx) key[n++] = rs[i];
		key[n++] = hasDepth; key[n++] = tw; key[n++] = th;
		for (int i = 0; i < 4; i++) key[n++] = dev.vp[i];
		memcpy(&key[n], dev.vpZ, 8); n += 2;
		for (int i = 0; i < 4; i++) key[n++] = (uint32_t)dev.scissor[i];
		while (n < 44) key[n++] = 0;
		if (gStValid && !memcmp(key, gStKey, sizeof key)) return;
		memcpy(gStKey, key, sizeof key); gStValid = true;
	}
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
		glBindTexture(target, t); gTexBindDirty = true;
		uint8_t px[4] = {0, 0, 0, 255};
		if (target == GL_TEXTURE_CUBE_MAP) for (int f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		else if (target == GL_TEXTURE_3D) glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	return t;
}

// ---- health report: things that make a draw differ from D3D9 without an error (logged once a minute when non-zero)
static uint32_t gHNoTex = 0, gHTexFail = 0, gHNoProg = 0;
static std::map<uint32_t, uint32_t> gHNoTexFmt;              // texture format -> draws where it could not be used
static std::map<std::pair<uint32_t, uint32_t>, uint32_t> gHState;   // (render state, value) the renderer ignores -> draws
static void healthStates()
{
	// render states we do not implement, compared with their D3D9 defaults
	static const std::pair<uint8_t, uint32_t> watch[] = {{8, 3}, {9, 2}, {28, 0}, {29, 0}, {48, 1}, {152, 0}, {156, 0}, {176, 0},
	                                                    {185, 0}, {186, 1}, {187, 1}, {188, 1}, {189, 8}, {190, 15}, {191, 15}, {192, 15}, {194, 0}};
	for (auto& w : watch) if (dev.rs[w.first] != w.second) gHState[{w.first, dev.rs[w.first]}]++;
}
static void healthReport()
{
	static int reports = 0;
	if (reports >= 20) return;
	std::string o;
	char b[96];
	if (gHNoProg) { snprintf(b, sizeof b, " | draws without a program %u", gHNoProg); o += b; }
	if (gHNoTex) { snprintf(b, sizeof b, " | sampler used with no texture bound %u", gHNoTex); o += b; }
	if (gHTexFail) { o += " | texture not usable (format:draws)"; for (auto& kv : gHNoTexFmt) { snprintf(b, sizeof b, " %u:%u", kv.first, kv.second); o += b; } }
	if (!gHState.empty()) { o += " | ignored render states (state=value:draws)"; for (auto& kv : gHState) { snprintf(b, sizeof b, " %u=%u:%u", kv.first.first, kv.first.second, kv.second); o += b; } }
	if (!o.empty()) { reports++; log("health (last 1800 frames)%s", o.c_str()); }
	gHNoTex = gHTexFail = gHNoProg = 0; gHNoTexFmt.clear(); gHState.clear();
}
static int activeUnit = -1;
void bindSamplers(Prog* p, ShaderObj* ps)
{
	// make every sampled texture current first: an upload binds on the active unit, which would otherwise
	// replace a texture bound for an earlier sampler of this same draw
	for (int i = 0; i < 16; i++)
		if (ps->info.samplerType[i] && p->s[i] >= 0 && dev.tex[i]) ensureTexture(dev.tex[i]);
	for (int i = 0; i < 16; i++)
	{
		int st = ps->info.samplerType[i];
		{
			static int nl3 = 0;
			if ((st || p->s[i] >= 0) && nl3++ < 40) log("GL: sampler slot %d psType %d loc %d", i, st, p->s[i]);
		}
		if (!st || p->s[i] < 0) continue;
		GLenum target = st == 3 ? GL_TEXTURE_CUBE_MAP : st == 4 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
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
		if (!name)
		{
			if (!t) gHNoTex++;
			else { gHTexFail++; gHNoTexFmt[texOwner(t)->fmt]++; }
		}
		GLuint bindName = name ? name : dummyTex(target);
		{
			// cached per unit: skip glActiveTexture/glBindTexture when the unit already holds this texture
			static GLuint boundTex[16][3]; static bool samplerBound[16];
			int ti = target == GL_TEXTURE_CUBE_MAP ? 1 : target == GL_TEXTURE_3D ? 2 : 0;
			if (gTexBindDirty) { memset(boundTex, 0xff, sizeof boundTex); memset(samplerBound, 0, sizeof samplerBound); gTexBindDirty = false; activeUnit = -1; }
			if (boundTex[i][ti] != bindName)
			{
				if (activeUnit != i) { glActiveTexture(GL_TEXTURE0 + i); activeUnit = i; }
				glBindTexture(target, bindName);
				if (gTexBindDirty) { memset(boundTex, 0xff, sizeof boundTex); memset(samplerBound, 0, sizeof samplerBound); gTexBindDirty = false; activeUnit = i; }
				boundTex[i][ti] = bindName;
			}
			if (name && texOwner(t)->mipDirty)
			{
				if (activeUnit != i) { glActiveTexture(GL_TEXTURE0 + i); activeUnit = i; }
				glGenerateMipmap(target);
				texOwner(t)->mipDirty = false;
			}
			if (!gSamplers[i]) glGenSamplers(1, &gSamplers[i]);
			if (!samplerBound[i]) { glBindSampler(i, gSamplers[i]); samplerBound[i] = true; }
		}
		if (!p->samplersSet) glUniform1i(p->s[i], i);
		const uint32_t* s = dev.samp[i];
		static GLenum cache[16][5];
		static bool cacheValid[16];
		auto sp = [&](int k, GLenum pname, GLenum v) { if (!cacheValid[i] || cache[i][k] != v) { glSamplerParameteri(gSamplers[i], pname, v); cache[i][k] = v; } };
		sp(0, GL_TEXTURE_WRAP_S, wrapMode(s[1]));
		sp(1, GL_TEXTURE_WRAP_T, wrapMode(s[2]));
		sp(2, GL_TEXTURE_WRAP_R, wrapMode(s[3]));
		if (gHasBorder && (s[1] == 4 || s[2] == 4 || s[3] == 4))
		{
			static uint32_t bc[16]; static bool bcSet[16];
			if (!bcSet[i] || bc[i] != s[4])
			{
				uint32_t c = s[4];
				GLfloat f[4] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, ((c >> 24) & 255) / 255.f};
				glSamplerParameterfv(gSamplers[i], 0x1004, f);   // GL_TEXTURE_BORDER_COLOR
				bc[i] = c; bcSet[i] = true;
			}
		}
		uint32_t mag = s[5], mn = s[6], mip = s[7];
		bool hasMips = t && texOwner(t)->levels > 1;
		GLenum minF;
		if (noFilter) minF = GL_NEAREST;
		else if (mn == 1) minF = (!hasMips || mip == 0) ? GL_NEAREST : mip == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
		else minF = (!hasMips || mip == 0) ? GL_LINEAR : mip == 1 ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
		sp(3, GL_TEXTURE_MIN_FILTER, minF);
		sp(4, GL_TEXTURE_MAG_FILTER, (noFilter || mag == 1) ? GL_NEAREST : GL_LINEAR);
		{
			// D3DTEXF_ANISOTROPIC (3) with D3DSAMP_MAXANISOTROPY: terrain and roads seen at the camera's angle
			static float aniso[16] = {0};
			float want = 1.f;
			if (!noFilter && gMaxAniso > 1.f && (mn == 3 || mag == 3)) want = std::max(1.f, std::min((float)std::max(1u, s[10]), gMaxAniso));
			if (aniso[i] != want) { glSamplerParameterf(gSamplers[i], 0x84FE, want); aniso[i] = want; }   // GL_TEXTURE_MAX_ANISOTROPY_EXT
		}
		cacheValid[i] = true;
	}
	p->samplersSet = true;
	if (activeUnit != 0) { glActiveTexture(GL_TEXTURE0); activeUnit = 0; }
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
void invalidateAttribCache();
void resizeBackBuffer() { gResizePending = true; }   // may be called from a thread without the GL context: applied by the GL thread

bool init()
{
	if (gInit) return gOk;
	gInit = true;
	if (!gfx::ready() || !gfx::ownedByGame()) { log("GL: no context for the game thread - drawing disabled"); return false; }
	const char* ext = (const char*)glGetString(GL_EXTENSIONS);
	gHasS3tc = ext && strstr(ext, "GL_EXT_texture_compression_s3tc");
	initBinCache();
	{
		GLint maj = 0, mnr = 0; glGetIntegerv(GL_MAJOR_VERSION, &maj); glGetIntegerv(GL_MINOR_VERSION, &mnr);
		gHasBorder = (maj > 3 || (maj == 3 && mnr >= 2)) || (ext && (strstr(ext, "_texture_border_clamp")));
		if (ext && strstr(ext, "GL_EXT_texture_filter_anisotropic")) { GLfloat m = 1.f; glGetFloatv(0x84FF, &m); gMaxAniso = m; }   // MAX_TEXTURE_MAX_ANISOTROPY
		log("GL: border clamp %d (GLES %d.%d), max anisotropy %g", (int)gHasBorder, maj, mnr, (double)gMaxAniso);
	}
	log("GL: %s | %s | s3tc %d | float rt %d", glGetString(GL_RENDERER), glGetString(GL_VERSION), (int)gHasS3tc, ext && strstr(ext, "EXT_color_buffer_float") != nullptr);
	glGenVertexArrays(1, &gVao);
	glGenFramebuffers(1, &gBackFbo);
	glGenFramebuffers(1, &gDrawFbo);
	glGenFramebuffers(1, &gReadFbo);
	glGenTextures(1, &gBackTex);
	glBindTexture(GL_TEXTURE_2D, gBackTex); gTexBindDirty = true;
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
static std::atomic<int> gRenderAcq{-1};
bool ensureGl()
{
	if (!gInit)
	{
		rec::gOn = access("/sdcard/ra3_st", F_OK) != 0;
		if (rec::gOn)
		{
			// a dedicated render thread owns the context; game threads only record commands
			rec::start([] { gRenderAcq = gfx::acquire() ? 1 : 0; });
			while (gRenderAcq.load() < 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			log("GL: first rendering call on tid %d, render thread acquire %d", (int)gettid(), gRenderAcq.load());
			if (gRenderAcq.load() != 1) { gInit = true; gOk = false; return false; }
			rec::s([] { init(); });
		}
		else
		{
			bool a = gfx::acquire();
			log("GL: first rendering call on tid %d (acquire %d), single-threaded", (int)gettid(), (int)a);
			init();
		}
	}
	else if (!rec::gOn && gOk && !gfx::isOwner()) gfx::acquire();   // the game moved rendering to another thread (e.g. fullscreen video)
	return gOk;
}
bool ready() { return gOk; }

// Frame trace (create /sdcard/ra3_frame): log every clear, copy and draw of frames 3600 and 5400.
static bool traceFrame()
{
	static bool on = access("/sdcard/ra3_frame", F_OK) == 0;
	return on && (gFrameNo == 3600 || gFrameNo == 5400);
}
// with /sdcard/ra3_dump present: save the target after every copy and full-screen pass of frame 5400
static bool traceDump()
{
	static bool on = access("/sdcard/ra3_dump", F_OK) == 0;
	return on && gFrameNo == 5400 && traceFrame();
}
static void dumpBmp(const char* path, uint32_t w, uint32_t h, uint32_t sc);
static void dumpTraced(const char* what, uint32_t w, uint32_t h)
{
	static uint32_t seq = 0;
	if (seq >= 40) return;
	char path[128]; snprintf(path, sizeof path, "/sdcard/ra3_dump/tr%02u_%s_%ux%u.bmp", seq++, what, w, h);
	dumpBmp(path, w, h, w > 1024 ? 2 : 1);
}
static std::string objTag(Obj* o)
{
	if (!o) return "back";
	char b[64]; snprintf(b, sizeof b, "%x(%s%ux%u f%u)", o->parent ? o->parent : o->addr, o->parent ? "s:" : "", o->w, o->h, o->fmt);
	return b;
}
void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
	if (!ensureGl()) return;
	bindTargets();
	uint32_t tw, th;
	targetSize(dev.rt0, tw, th);
	gStValid = false;   // clear changes scissor/masks; bindings stay
	glDisable(GL_SCISSOR_TEST);
	GLbitfield m = 0;
	if (flags & 1) { glColorMask(1, 1, 1, 1); glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f); m |= GL_COLOR_BUFFER_BIT; }
	bool hasDepth = isBack(dev.rt0) || dev.ds;
	if ((flags & 2) && hasDepth) { glDepthMask(GL_TRUE); glClearDepthf(z); m |= GL_DEPTH_BUFFER_BIT; }
	if ((flags & 4) && hasDepth) { glStencilMask(0xff); glClearStencil((GLint)stencil); m |= GL_STENCIL_BUFFER_BIT; }
	if (traceFrame()) log("TR clear rt %s flags %x color %08x z %g rects %u", objTag(dev.rt0).c_str(), flags, color, (double)z, count);
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
					rec::scall(pBufStorage, target, (GLsizeiptr)cap, (const void*)nullptr, fl);
					uint8_t* p = (uint8_t*)glMapBufferRange(target, 0, cap, fl);
					if (p && glGetError() == GL_NO_ERROR) { glDeleteBuffers(1, &buf); invalidateAttribCache(); buf = nb; pr.ptr = p; pr.ok = true; log("GL: persistent ring %u KB target %x", cap >> 10, target); }
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
		int left = pr.seg;
		bool change = segNow != pr.seg;
		pr.seg = segNow;
		uint32_t at = pos + headBytes;
		// entering a new segment: fence the one we left, wait for the GPU to finish the one we enter. With the render
		// thread the copy and the fences run there, in order (the fences and the mapping belong to that thread).
		auto put = [&pr, left, segNow, change, at](const uint8_t* src, uint32_t n)
		{
			if (change)
			{
				if (pr.fence[left]) glDeleteSync(pr.fence[left]);
				pr.fence[left] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
				if (pr.fence[segNow])
				{
					glClientWaitSync(pr.fence[segNow], GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull);
					glDeleteSync(pr.fence[segNow]);
					pr.fence[segNow] = nullptr;
				}
			}
			if (n) memcpy(pr.ptr + at, src, n);
		};
		if (rec::active())
		{
			PRing* prp = &pr;
			rec::qd(data, bytes, [prp, left, segNow, change, at, bytes](const uint8_t* cp)
			{
				return [prp, left, segNow, change, at, bytes, cp]
				{
					PRing& r = *prp;
					if (change)
					{
						if (r.fence[left]) glDeleteSync(r.fence[left]);
						r.fence[left] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
						if (r.fence[segNow]) { glClientWaitSync(r.fence[segNow], GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull); glDeleteSync(r.fence[segNow]); r.fence[segNow] = nullptr; }
					}
					if (bytes) memcpy(r.ptr + at, cp, bytes);
				};
			});
		}
		else put((const uint8_t*)data, bytes);
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
static uint64_t tProg, tState, tUni, tSamp, tAttr, tDraw, nDraw;

static bool gDiagPair = false;   // set per draw: first draws of each new shader pair during gameplay
static bool isDiagFrame() { return verbose() && (gFrameNo == 700 || gFrameNo == 1800 || gFrameNo == 3600 || gDiagPair); }
static void dumpBmp(const char* path, uint32_t w, uint32_t h, uint32_t sc = 2)
{
	if (!w || !h || w > 4096 || h > 4096) return;
	std::vector<uint8_t> px((size_t)w * h * 4);
	glFinish();
	GLint prevRead = 0, curDraw = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curDraw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, curDraw);   // read what was just drawn
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
	// scaled-down 24-bit BMP; GL row 0 is the top of the D3D image, BMP rows are stored bottom-up
	uint32_t ow = w / sc, oh = h / sc, rowB = (ow * 3 + 3) & ~3u, imgB = rowB * oh;
	std::vector<uint8_t> bmp(54 + imgB, 0);
	auto w32 = [&](size_t o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
	bmp[0] = 'B'; bmp[1] = 'M'; w32(2, 54 + imgB); w32(10, 54); w32(14, 40); w32(18, ow); w32(22, oh); bmp[26] = 1; bmp[28] = 24; w32(34, imgB);
	for (uint32_t y = 0; y < oh; y++)
		for (uint32_t x = 0; x < ow; x++)
		{
			const uint8_t* q = &px[((size_t)(sc * y) * w + sc * x) * 4];
			uint8_t* o = &bmp[54 + (size_t)(oh - 1 - y) * rowB + x * 3];
			o[0] = q[2]; o[1] = q[1]; o[2] = q[0];
		}
	if (FILE* f = fopen(path, "wb")) { fwrite(bmp.data(), 1, bmp.size(), f); fclose(f); log("dumped %s (%ux%u)", path, ow, oh); }
	else log("dump failed: %s", path);
}

// Vertex attribute state of gVao, cached so unchanged attributes cost no GL calls.
struct AttrState { GLuint buf; GLint size; GLenum type; GLboolean norm; GLsizei stride; uintptr_t off; GLuint div; int8_t on; };   // on: 1 enabled, 0 disabled, -1 unknown
static AttrState gAttr[16];
static bool gAttrValid = false;
void invalidateAttribCache() { gAttrDirty = true; }
static void setAttrib(int loc, GLuint buf, GLint size, GLenum type, bool norm, GLsizei stride, uintptr_t off, GLuint div)
{
	if (gAttrDirty) { gAttrValid = false; gAttrDirty = false; }
	if (!gAttrValid) { for (auto& a : gAttr) a = AttrState{~0u, 0, 0, 0, -1, ~(uintptr_t)0, ~0u, -1}; gAttrValid = true; }
	AttrState& a = gAttr[loc];
	if (a.on != 1) { glEnableVertexAttribArray(loc); a.on = 1; }
	if (a.buf != buf || a.size != size || a.type != type || a.norm != (GLboolean)norm || a.stride != stride || a.off != off)
	{
		glBindBuffer(GL_ARRAY_BUFFER, buf);
		glVertexAttribPointer(loc, size, type, norm, stride, (const void*)off);
		a.buf = buf; a.size = size; a.type = type; a.norm = norm; a.stride = stride; a.off = off;
	}
	if (a.div != div) { glVertexAttribDivisor(loc, div); a.div = div; }
}
static void disableAttribsExcept(uint32_t enabled)
{
	if (gAttrDirty) { gAttrValid = false; gAttrDirty = false; }
	if (!gAttrValid) { for (auto& a : gAttr) a = AttrState{~0u, 0, 0, 0, -1, ~(uintptr_t)0, ~0u, -1}; gAttrValid = true; }
	for (int loc = 0; loc < 16; loc++)
		if (!(enabled >> loc & 1) && gAttr[loc].on != 0)
		{
			glDisableVertexAttribArray(loc); glVertexAttrib4f(loc, 0.f, 0.f, 0.f, 1.f);
			gAttr[loc].on = 0;
		}
}

static uint32_t gFxDraws[512 * 8], gFxPrims[512 * 8];
static void drawImpl(const DrawCall& dc);

// ---- interface scaling
// The game lays its HUD out in back-buffer pixels at a size proportional to the screen, which is small on a phone. HUD
// pieces are drawn by the interface shader (APT); every such draw that lies entirely in one corner zone of the screen
// (portrait, minimap, build menu, power bar) is scaled up around that screen corner. Touches on the enlarged panels are
// mapped back to the game's own coordinates (rt::uiUnmap).
namespace
{
const uint64_t kAptVs = 0xe22700c621ded1fbull;   // interface vertex shader: pixel positions, transform in c11..c14
const uint64_t kVideoVs = 0xcb5e040244f89931ull; // video.fx (commander video on the minimap): positions 0..1 of the screen
struct UiRect { float x0, y0, x1, y1, ax, ay, sc; };     // scaled rectangle and its anchor, back-buffer pixels
std::vector<UiRect> gUiCur, gUiPub;
std::mutex gUiMu;
float gUiScale = 1.f;
}
void uiUnmapImpl(int& x, int& y)
{
	std::lock_guard<std::mutex> l(gUiMu);
	if (gUiScale <= 1.001f) return;
	for (size_t i = gUiPub.size(); i-- > 0;)
	{
		const UiRect& r = gUiPub[i];
		if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
		x = (int)lroundf(r.ax + (x - r.ax) / r.sc);
		y = (int)lroundf(r.ay + (y - r.ay) / r.sc);
		return;
	}
}
// Interface scaling of one draw. Returns the scale-1 to apply with the u_ui uniform when the whole draw lies in one
// corner zone (anchor in clip space). A draw with pieces in several zones (text of the whole HUD batched into one
// draw) is rebuilt on the CPU instead: every triangle in a corner zone is moved there, the rest stays; then `split`
// holds the vertices for a non-indexed triangle list.
static bool gUiDiag = false;
static bool uiDiagFrame() { return gUiDiag; }   // a few frames during play: every interface draw into the log
static bool gUiAptSeen = false;   // an interface draw happened this frame (diagnostics)
static bool gUiHud = false;       // the HUD panels have started this frame: earlier interface draws belong to the world
static float gUiColLeft = 1e9f, gUiCurColLeft = 1e9f;   // left edge of the right column (back-buffer pixels)
static float gUiRightT = 0.f, gUiRightB = 1.f, gUiCurT = 0.f, gUiCurB = 1.f;   // right column: lowest top piece, highest bottom piece (fractions of the height)
static std::vector<UiRect> gUiPieces;   // panel pieces scaled this frame, in the game's own (unscaled) coordinates
static bool gUiMenu = false;      // a big window covers the battle this frame
static bool gUiNested = false;     // drawing a rebuilt interface draw (no second analysis)
static uint32_t gUiWorldDraws = 0;   // depth-tested draws this frame: the HUD is scaled in a battle only, not in menus
static float uiScaleFor(const DrawCall& dc, const DeclElem* els, uint32_t nEls, float& axN, float& ayN, float& ax, float& ay, std::vector<uint8_t>& split)
{
	if (gUiNested || gUiScale <= 1.001f || gUiWorldDraws < 50 || !dev.vs || !isBack(dev.rt0)) return 0.f;
	if (gUiDiag && gUiAptSeen)
	{
		// every back-buffer draw after the first interface draw: which shaders draw the rest (texts)
		std::string cn;
		for (auto& cc : dev.vs->info.ctab) if (cc.regSet != 3 && cn.size() < 80) { cn += " "; cn += cc.name; }
		log("ui-diag: draw vs %016llx z %u prim %u n %u up %d consts:%s", (unsigned long long)dev.vs->hash, dev.rs[7], dc.prim, dc.primCount, dc.upVerts ? 1 : 0, cn.c_str());
	}
	if (dev.vs->hash == kAptVs) gUiAptSeen = true;
	if (dev.rs[7]) return 0.f;
	// the interface shader (World matrix in c11..c14) or a shader without constants that passes clip-space positions
	// through (the commander video on the minimap); anything else is left alone
	bool apt = dev.vs->hash == kAptVs, pass = false, video = dev.vs->hash == kVideoVs;
	if (!apt && !video)
	{
		pass = true;
		for (auto& cc : dev.vs->info.ctab) if (cc.regSet != 3) { pass = false; break; }
		if (!dev.vs->info.defF.empty()) pass = false;
	}
	bool diag = uiDiagFrame();
	if (!apt && !pass && !video) { if (diag) log("ui-diag: vs %016llx prim %u n %u: not an interface shader", (unsigned long long)dev.vs->hash, dc.prim, dc.primCount); return 0.f; }
	const DeclElem* pe = nullptr;
	for (uint32_t i = 0; i < nEls; i++) if (els[i].usage == 0 && els[i].index == 0 && (els[i].type >= 1 && els[i].type <= 3)) { pe = &els[i]; break; }
	auto fail = [&](const char* why) { if (diag) log("ui-diag: vs %016llx prim %u n %u skipped: %s", (unsigned long long)dev.vs->hash, dc.prim, dc.primCount, why); return 0.f; };
	if (!pe) { if (diag) { std::string t; for (uint32_t q = 0; q < nEls; q++) { char b[32]; snprintf(b, sizeof b, " u%u.%u:t%u", els[q].usage, els[q].index, els[q].type); t += b; } log("ui-diag: no position element:%s", t.c_str()); } return 0.f; }
	const uint8_t* vb; uint32_t stride; uint64_t vbSize;
	if (dc.upVerts) { vb = (const uint8_t*)dc.upVerts; stride = dc.upStride; vbSize = ~0ull; }
	else
	{
		const Stream& st = dev.streams[pe->stream < 16 ? pe->stream : 0];
		if (!st.vb || !st.vb->data || !st.stride) return fail("no vertex data");
		vb = (const uint8_t*)guest::host(st.vb->data) + st.offset; stride = st.stride; vbSize = st.vb->size > st.offset ? st.vb->size - st.offset : 0;
	}
	uint32_t nIdx = (uint32_t)primVertices(dc.prim, dc.primCount);
	if (!nIdx || nIdx > 6000) return fail("vertex count");
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	if (apt) memcpy(m, dev.vsF + 4 * 11, sizeof m);
	if (video) { const float mv[16] = {2, 0, 0, -1, 0, -2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}; memcpy(m, mv, sizeof m); }   // video.fx: positions 0..1
	if (m[12] != 0.f || m[13] != 0.f) return fail("perspective");   // w must not depend on x, y (flat interface)
	const bool pos4 = pe->type == 3;
	const size_t posBytes = pe->type == 1 ? 8 : pos4 ? 16 : 12;
	float det = m[0] * m[5] - m[1] * m[4];
	if (fabsf(det) < 1e-12f) return fail("singular matrix");
	uint32_t vx = dev.vp[0], vy = dev.vp[1], vw = dev.vp[2], vh = dev.vp[3];
	if (!vw || !vh) { vx = vy = 0; targetSize(nullptr, vw, vh); }
	const float W = (float)vw, H = (float)vh;
	const uint8_t* ip = nullptr; bool i32 = false;
	if (dc.indexed)
	{
		if (dc.upIdx) { ip = (const uint8_t*)dc.upIdx; i32 = dc.upIdxFmt == 102; }
		else if (dev.ib && dev.ib->data) { i32 = dev.ib->fmt == 102; if ((uint64_t)(dc.startIndex + nIdx) * (i32 ? 4 : 2) > dev.ib->size) return fail("index range"); ip = (const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * (i32 ? 4 : 2); }
		else return fail("no index buffer");
	}
	static std::vector<int64_t> vix; static std::vector<float> sxy, wv;
	vix.resize(nIdx); sxy.resize(nIdx * 2); wv.resize(nIdx);
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (uint32_t i = 0; i < nIdx; i++)
	{
		int64_t v;
		if (ip) { uint32_t k; if (i32) memcpy(&k, ip + 4 * i, 4); else { uint16_t s16; memcpy(&s16, ip + 2 * i, 2); k = s16; } v = (int64_t)dc.baseVertex + k; }
		else v = (int64_t)dc.baseVertex + i;
		if (v < 0 || (uint64_t)v * stride + pe->offset + posBytes > vbSize) return fail("vertex outside the buffer");
		float p[4] = {0, 0, 0, 1}; memcpy(p, vb + (uint64_t)v * stride + pe->offset, posBytes);
		float cx = p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + p[3] * m[3];
		float cy = p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + p[3] * m[7];
		float cw = p[2] * m[14] + p[3] * m[15];
		if (fabsf(cw) < 1e-6f) return fail("w = 0");
		float sx = vx + (cx / cw + 1.f) * 0.5f * W, sy = vy + (1.f - cy / cw) * 0.5f * H;
		vix[i] = v; sxy[2 * i] = sx; sxy[2 * i + 1] = sy; wv[i] = cw;
		x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
	}
	// the right column (minimap + build menu from the top, unit panel from the bottom) has to fit the screen height:
	// its scale is limited so the two groups do not overlap (extents from the previous frame)
	auto scaleOf = [&](int c) -> float {
		if ((c != 2 && c != 4) || gUiRightT <= 0.f || gUiRightB >= 1.f) return gUiScale;
		return std::max(1.f, std::min(gUiScale, 1.f / (gUiRightT + 1.f - gUiRightB) * 0.99f));
	};
	// only pieces of the column itself (not the command bar that opens to the left of the unit panel)
	auto noteRight = [&](int c, float a0, float b0, float a1, float b1) {
		if (a0 < vx + 0.75f * W) return;
		if (c == 2) gUiCurT = std::max(gUiCurT, (b1 - vy) / H);
		if (c == 4 && a1 >= vx + 0.97f * W) gUiCurB = std::min(gUiCurB, (b0 - vy) / H);
	};
	// corner of a rectangle: 0 none, else 1 + (right) + 2 * (bottom)
	// corner zones; on the right the minimap column with the open build menu reaches two thirds down the screen
	auto corner = [&](float a0, float b0, float a1, float b1) -> int {
		// the unit panel's command bar opens to the left along the bottom edge
		if (a0 >= vx + 0.50f * W && b0 >= vy + 0.86f * H) return 4;
		bool l = a1 <= vx + 0.40f * W, r = a0 >= vx + 0.75f * W;
		if (!(l || r)) return 0;
		bool t = b1 <= vy + (r ? 0.66f : 0.45f) * H, b = b0 >= vy + (r ? 0.70f : 0.55f) * H;
		if (!(t || b)) return 0;
		return 1 + (r ? 1 : 0) + (b ? 2 : 0);
	};
	auto anchorOf = [&](int c, float& cx, float& cy) { cx = (c - 1) & 1 ? (float)vx + W : (float)vx; cy = (c - 1) & 2 ? (float)vy + H : (float)vy; };
	auto addRect = [&](float a0, float b0, float a1, float b1, float cx, float cy, float s) { if (gUiCur.size() < 4000) gUiCur.push_back({cx + (a0 - cx) * s, cy + (b0 - cy) * s, cx + (a1 - cx) * s, cy + (b1 - cy) * s, cx, cy, s}); };
	int c = corner(x0, y0, x1, y1);
	// a tooltip or popup that ends at the left edge of the right column belongs to it (it is anchored there)
	bool attached = !c && gUiColLeft < 1e8f && x1 >= gUiColLeft - 0.04f * W && x0 < gUiColLeft && x1 <= gUiColLeft + 0.20f * W && y1 <= vy + 0.75f * H;
	if (attached) c = 2;
	if (c == 2 && !attached) gUiCurColLeft = std::min(gUiCurColLeft, x0);
	// unit markers, health bars and names come first in a frame: only what follows the first big corner panel is HUD
	if (!gUiHud && c && (x1 - x0) * (y1 - y0) >= 0.01f * W * H) gUiHud = true;
	if (!gUiHud) { if (diag) log("ui-diag: vs %016llx box %.0f,%.0f-%.0f,%.0f before the HUD", (unsigned long long)dev.vs->hash, x0, y0, x1, y1); return 0.f; }
	// a large window over the battle (pause / episode menu): nothing more is scaled this frame, and touches go to the
	// game unchanged (the HUD under it is covered)
	if (gUiMenu) return 0.f;
	if (apt && (x1 - x0) * (y1 - y0) >= 0.35f * W * H && (x1 - x0) < 0.98f * W)
	{
		gUiMenu = true; gUiCur.clear(); gUiPieces.clear();
		if (diag) log("ui-diag: big window %.0f,%.0f-%.0f,%.0f: scaling off for this frame", x0, y0, x1, y1);
		return 0.f;
	}
	if (diag) log("ui-diag: vs %016llx%s prim %u n %u idx %u box %.0f,%.0f-%.0f,%.0f corner %d", (unsigned long long)dev.vs->hash, apt ? " (apt)" : " (pass)", dc.prim, dc.primCount, nIdx, x0, y0, x1, y1, c);
	if (c)
	{
		anchorOf(c, ax, ay);
		if (!attached) noteRight(c, x0, y0, x1, y1);
		float s = scaleOf(c);
		addRect(x0, y0, x1, y1, ax, ay, s);
		// a tooltip's text can start left of its box: texts get a wider catch area to the left of attached popups
		if (gUiPieces.size() < 2000) gUiPieces.push_back({attached ? x0 - 0.15f * W : x0, y0, x1, y1, ax, ay, s});
		axN = (c - 1) & 1 ? 1.f : -1.f; ayN = (c - 1) & 2 ? -1.f : 1.f;
		return s - 1.f;
	}
	if (dc.prim != 4 || nIdx % 3) return fail("mixed corners, not a triangle list");
	// mixed draw (the texts of the whole HUD in one batch): a triangle that lies on a panel scaled earlier in this frame
	// moves with that panel; texts elsewhere (subtitles, unit names) stay
	auto pieceOf = [&](float a0, float b0, float a1, float b1) -> const UiRect* {
		float cxm = (a0 + a1) * 0.5f, cym = (b0 + b1) * 0.5f;
		for (size_t q = gUiPieces.size(); q-- > 0;)
		{
			const UiRect& r = gUiPieces[q];
			if (cxm >= r.x0 && cxm <= r.x1 && cym >= r.y0 && cym <= r.y1) return &r;
		}
		return nullptr;
	};
	bool any = false;
	for (uint32_t t = 0; t < nIdx && !any; t += 3)
	{
		float a0 = std::min({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]}), a1 = std::max({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]});
		float b0 = std::min({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]}), b1 = std::max({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]});
		any = pieceOf(a0, b0, a1, b1) != nullptr;
	}
	if (!any) return fail("mixed draw, nothing on a panel");
	split.resize((size_t)nIdx * stride);
	for (uint32_t t = 0; t < nIdx; t += 3)
	{
		float a0 = std::min({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]}), a1 = std::max({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]});
		float b0 = std::min({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]}), b1 = std::max({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]});
		const UiRect* pr = pieceOf(a0, b0, a1, b1);
		bool tc = pr != nullptr;
		float cx = 0, cy = 0, s = 1.f;
		if (tc) { cx = pr->ax; cy = pr->ay; s = pr->sc; }
		for (uint32_t k = t; k < t + 3; k++)
		{
			uint8_t* d = split.data() + (size_t)k * stride;
			{ uint64_t at = (uint64_t)vix[k] * stride; memset(d, 0, stride); memcpy(d, vb + at, (size_t)std::min<uint64_t>(stride, vbSize - at)); }
			if (!tc) continue;
			// new screen position -> clip space -> object x, y (z kept)
			float nx = cx + (sxy[2 * k] - cx) * s, ny = cy + (sxy[2 * k + 1] - cy) * s;
			float w = wv[k];
			float qx = ((nx - vx) / W * 2.f - 1.f) * w, qy = (1.f - (ny - vy) / H * 2.f) * w;
			float p[4] = {0, 0, 0, 1}; memcpy(p, d + pe->offset, posBytes);
			float rx = qx - m[2] * p[2] - m[3] * p[3], ry = qy - m[6] * p[2] - m[7] * p[3];
			p[0] = (rx * m[5] - m[1] * ry) / det;
			p[1] = (m[0] * ry - rx * m[4]) / det;
			memcpy(d + pe->offset, p, 8);
		}
	}
	return 0.f;
}
void draw(const DrawCall& dc)
{
	// time one draw in 8 (clock reads are not free at ~1000 draws per frame)
	if ((gNDraws++ & 7) == 0) { uint64_t t0 = nowUs(); drawImpl(dc); gTDraw += (nowUs() - t0) * 8; }
	else drawImpl(dc);
}
static void drawImpl(const DrawCall& dc)
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
	uint64_t t0 = profNow();
	Prog* p = getProgram(vs, ps);
	if (!p) { gHNoProg++; static int nf = 0; if (nf++ < 10) log("d3d9: draw skipped: no program for vs %016llx ps %016llx", (unsigned long long)vs->hash, (unsigned long long)ps->hash); return; }
	healthStates();
	if (dev.rs[7] && !gUiNested) gUiWorldDraws++;   // ZENABLE
	if (gUiDiag && !gUiNested)
	{
		static int nd = 0;
		if (nd++ < 1500) log("ui-diag: any vs %016llx ps %016llx rt %s z %u prim %u n %u up %d", (unsigned long long)vs->hash, (unsigned long long)ps->hash, isBack(dev.rt0) ? "back" : objTag(dev.rt0).c_str(), dev.rs[7], dc.prim, dc.primCount, dc.upVerts ? 1 : 0);
	}
	if (fxStatOn() && gFxDiagCur >= 0 && gFxDiagCur < 512) { gFxDraws[gFxDiagCur * 8 + (gFxDiagTech & 7)]++; gFxPrims[gFxDiagCur * 8 + (gFxDiagTech & 7)] += dc.primCount; }
	if (traceFrame())
	{
		std::string tx; bool anyRt = false;
		for (int i = 0; i < 16; i++)
			if (Obj* t = dev.tex[i])
			{
				char b[128]; snprintf(b, sizeof b, " t%d=%s%s a%u/%u f%u/%u", i, objTag(t).c_str(), t->rtTex ? "RT" : "", dev.samp[i][1], dev.samp[i][2], dev.samp[i][5], dev.samp[i][6]);
				tx += b; anyRt |= t->rtTex;
			}
		uint32_t rw = 0, rh = 0; targetSize(dev.rt0, rw, rh);
		log("TR draw vs %016llx ps %016llx prim %u n %u rt %s ds %s vp %u,%u,%u,%u sc %u[%d,%d,%d,%d] bl %u %u/%u z %u/%u cw %x |%s", (unsigned long long)vs->hash, (unsigned long long)ps->hash, dc.prim, dc.primCount,
			objTag(dev.rt0).c_str(), dev.ds ? objTag(dev.ds).c_str() : "-", dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3], dev.rs[174], dev.scissor[0], dev.scissor[1], dev.scissor[2], dev.scissor[3],
			dev.rs[27], dev.rs[19], dev.rs[20], dev.rs[7], dev.rs[14], dev.rs[168], tx.c_str());
		static std::set<std::pair<uint64_t, uint64_t>> srcSeen;
		if (anyRt && srcSeen.size() < 24 && srcSeen.insert({vs->hash, ps->hash}).second)
		{
			log("TR VS source:\n%s", shdr::compose(vs->info).c_str());
			log("TR PS source:\n%s", shdr::compose(ps->info, &vs->info.outputs).c_str());
			for (int pass = 0; pass < 2; pass++)
				for (auto& cc : (pass == 0 ? vs->info.ctab : ps->info.ctab))
				{
					if (cc.regSet != 2) continue;
					const float* base = pass == 0 ? dev.vsF : dev.psF;
					std::string vv;
					for (uint32_t r = 0; r < std::min<uint32_t>(cc.regCount, 16) && cc.regIndex + r < (pass == 0 ? 256u : 224u); r++)
						{ char b5[96]; const float* f = base + 4 * (cc.regIndex + r); snprintf(b5, sizeof b5, " [%g %g %g %g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3]); vv += b5; }
					log("TR %s c %s r%u x%u:%s", pass == 0 ? "VS" : "PS", cc.name.c_str(), cc.regIndex, cc.regCount, vv.c_str());
				}
		}
	}
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
		static bool diagOn = verbose() && access("/sdcard/ra3_diag", F_OK) == 0;
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
	{
		// Effects diagnostics (create /sdcard/ra3_fxdiag): describe the first draws of every alpha-blended
		// shader pair during gameplay - state, textures, vertex layout, raw vertices and constants.
		static bool fxOn = access("/sdcard/ra3_fxdiag", F_OK) == 0;
		static std::map<std::tuple<uint64_t, uint64_t, uint32_t>, int> fxSeen;
		static int fxLogged = 0, fxSrc = 0;
		// with /sdcard/ra3_fxstat: the first draws of the laser effect (laser.fxo, blob 11012 bytes)
		static int laserLogged = 0;
		bool laserDiag = fxStatOn() && gFxDiagBlob == 11012 && gFrameNo >= 1500 && laserLogged < 6;
		if (laserDiag) fxSeen.erase({vs->hash, ps->hash, (dev.rs[19] & 15) | (dev.rs[20] & 15) << 4 | (dc.upVerts ? 256u : 0u)}), laserLogged++;
		if ((fxOn && gFrameNo >= 1500 && dev.rs[27] && fxLogged < 160) || laserDiag)
		{
			uint32_t key = (dev.rs[19] & 15) | (dev.rs[20] & 15) << 4 | (dc.upVerts ? 256u : 0u);
			int& n = fxSeen[{vs->hash, ps->hash, key}];
			if (n < 2)
			{
				bool first = n == 0;
				n++; fxLogged++;
				uint32_t rw = 0, rh = 0; targetSize(dev.rt0, rw, rh);
				log("FX #%d frame %u vs %016llx ps %016llx prim %u cnt %u idx %d start %u base %d min %u nv %u up %d | z %u zw %u zfunc %u src %u dst %u cull %u atest %u aref %u cw %x | rt %s %ux%u vp %u,%u,%u,%u",
					fxLogged, gFrameNo, (unsigned long long)vs->hash, (unsigned long long)ps->hash, dc.prim, dc.primCount, (int)dc.indexed, dc.startIndex, (int)dc.baseVertex, dc.minIndex, dc.numVertices, dc.upVerts ? 1 : 0,
					dev.rs[7], dev.rs[14], dev.rs[23], dev.rs[19], dev.rs[20], dev.rs[22], dev.rs[15], dev.rs[24], dev.rs[168], isBack(dev.rt0) ? "back" : "tex", rw, rh, dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3]);
				std::string tx;
				for (int i = 0; i < 8; i++)
					if (Obj* t = dev.tex[i])
					{
						unsigned nz = 0;
						if (t->data && t->size) { const uint8_t* b = (const uint8_t*)guest::host(t->data); for (uint32_t k = 0; k < std::min<uint32_t>(t->size, 8192); k += 5) nz += b[k] != 0; }
						char b2[160]; snprintf(b2, sizeof b2, " t%d=%ux%u/f%u/l%u/nz%u%s%s[a%u/%u f%u/%u/%u]", i, t->w, t->h, t->fmt, t->levels, nz, t->rtTex ? "/RT" : "", t->data ? "" : "/nodata", dev.samp[i][1], dev.samp[i][2], dev.samp[i][5], dev.samp[i][6], dev.samp[i][7]);
						tx += b2;
					}
				log("FX   tex:%s", tx.c_str());
				if (dev.vdecl)
				{
					std::string de;
					for (size_t i = 0; i < dev.vdecl->blob.size() / 8; i++)
					{
						DeclElem e; memcpy(&e, dev.vdecl->blob.data() + 8 * i, 8);
						if (e.stream == 0xff) break;
						char b3[48]; snprintf(b3, sizeof b3, " s%u+%u:t%u:u%u.%u", e.stream, e.offset, e.type, e.usage, e.index); de += b3;
					}
					log("FX   decl:%s", de.c_str());
				}
				for (int si = 0; si < 4; si++)
				{
					Stream& st = dev.streams[si];
					if (!st.vb) continue;
					std::string o;
					char hb[96]; snprintf(hb, sizeof hb, " s%d size %u stride %u off %u freq %x usage %x%s:", si, st.vb->size, st.stride, st.offset, st.freq, st.vb->usage, st.vb->data ? "" : " NODATA");
					o += hb;
					if (st.vb->data && st.stride)
						for (uint32_t k = 0; k < 4; k++)
						{
							int64_t vi = dc.baseVertex;
							if (dc.indexed && dev.ib && dev.ib->data)
							{
								uint32_t isz = dev.ib->fmt == 102 ? 4 : 2;
								if ((uint64_t)(dc.startIndex + k + 1) * isz > dev.ib->size) break;
								const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data);
								vi += isz == 4 ? ((const uint32_t*)ip)[dc.startIndex + k] : ((const uint16_t*)ip)[dc.startIndex + k];
							}
							else vi += dc.startIndex + k;
							uint64_t off = st.offset + (uint64_t)vi * st.stride;
							if (vi < 0 || off + st.stride > st.vb->size) { o += " (out of range)"; break; }
							const uint8_t* vp = (const uint8_t*)guest::host(st.vb->data) + off;
							char vb2[32]; snprintf(vb2, sizeof vb2, " v%lld[", (long long)vi); o += vb2;
							for (uint32_t b = 0; b + 4 <= st.stride && b < 64; b += 4) { float f; memcpy(&f, vp + b, 4); uint32_t u; memcpy(&u, vp + b, 4); char fb[40]; bool fl = u == 0 || (fabsf(f) > 1e-4f && fabsf(f) < 1e7f); if (fl) snprintf(fb, sizeof fb, "%g ", (double)f); else snprintf(fb, sizeof fb, "x%08x ", u); o += fb; }
							o += "]";
						}
					log("FX   stream%s", o.c_str());
				}
				if (first)
				{
					for (int pass = 0; pass < 2; pass++)
					{
						auto& ct = pass == 0 ? vs->info.ctab : ps->info.ctab;
						const float* base = pass == 0 ? dev.vsF : dev.psF;
						uint32_t maxReg = pass == 0 ? 256 : 224;
						for (auto& cc : ct)
						{
							if (cc.regSet != 2 || cc.regIndex >= maxReg) continue;
							std::string vv;
							for (uint32_t r = 0; r < std::min<uint32_t>(cc.regCount, 4) && cc.regIndex + r < maxReg; r++)
								{ char b5[96]; const float* f = base + 4 * (cc.regIndex + r); snprintf(b5, sizeof b5, " [%g %g %g %g]", (double)f[0], (double)f[1], (double)f[2], (double)f[3]); vv += b5; }
							log("FX   %s c %s r%u x%u:%s", pass == 0 ? "VS" : "PS", cc.name.c_str(), cc.regIndex, cc.regCount, vv.c_str());
						}
					}
					if (fxSrc++ < 14)
					{
						log("FX VS source:\n%s", shdr::compose(vs->info).c_str());
						log("FX PS source:\n%s", shdr::compose(ps->info, &vs->info.outputs).c_str());
					}
				}
			}
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
	{
		// /sdcard/ra3_nocache: re-send all GL state every draw (to rule the state caches in or out)
		static const bool noCache = access("/sdcard/ra3_nocache", F_OK) == 0;
		if (noCache) invalidateGlState();
	}
	bindTargets();
	uint32_t tw, th;
	targetSize(dev.rt0, tw, th);
	uint64_t t1 = profNow();
	applyState(tw, th);
	if (gCurProg != p->id) { glUseProgram(p->id); gCurProg = p->id; }
	uint64_t t2 = profNow();
	if (!gVaoBound) { glBindVertexArray(gVao); gVaoBound = true; }
	// constants
	{
		const float* vsrc = dev.vsF;
		int nv = std::max(1, std::min(256, vs->info.numC + 0));
		if (vs->info.relC && !vs->info.defF.empty())
		{
			// D3D: constants a shader defines itself (def c#) live in the constant registers, so relative
			// addressing (c[a0 + n]) must see them - e.g. the random and corner tables of the particle shaders.
			static float merged[256 * 4];
			memcpy(merged, dev.vsF, sizeof merged);
			for (auto& d : vs->info.defF) if (d.first >= 0 && d.first < 256) memcpy(merged + 4 * d.first, d.second.data(), 16);
			vsrc = merged; nv = 256;
		}
		uploadVec4Range(p->vc, p->vcLoc, p->vcShadow, vsrc, nv, p->id, "vc", p->shadowValid);
		uploadVec4Range(p->pc, p->pcLoc, p->pcShadow, dev.psF, std::max(1, std::min(224, ps->info.numC + 0)), p->id, "pc", p->shadowValid);
		if (p->vci >= 0 && (!p->shadowValid || memcmp(p->vciShadow, dev.vsI, 64 * 4))) { glUniform4iv(p->vci, 16, dev.vsI); memcpy(p->vciShadow, dev.vsI, 64 * 4); }
		if (p->vcb >= 0 && (!p->shadowValid || memcmp(p->vcbShadow, dev.vsB, 16 * 4))) { glUniform1iv(p->vcb, 16, dev.vsB); memcpy(p->vcbShadow, dev.vsB, 16 * 4); }
		if (p->pci >= 0 && (!p->shadowValid || memcmp(p->psiShadow, dev.psI, 64 * 4))) { glUniform4iv(p->pci, 16, dev.psI); memcpy(p->psiShadow, dev.psI, 64 * 4); }
		if (p->pcb >= 0 && (!p->shadowValid || memcmp(p->pcbShadow, dev.psB, 16 * 4))) { glUniform1iv(p->pcb, 16, dev.psB); memcpy(p->pcbShadow, dev.psB, 16 * 4); }
		if (p->flip >= 0 && (!p->shadowValid || p->flipShadow != -1.f)) { glUniform1f(p->flip, -1.f); p->flipShadow = -1.f; }
		if (p->hp >= 0)
		{
			// D3D9 puts pixel centres on integer coordinates, GL half a pixel further: shift geometry by half a pixel
			// (right and down) so full-screen passes cover the last row/column exactly as on D3D9
			uint32_t vw = dev.vp[2] ? dev.vp[2] : tw, vh = dev.vp[3] ? dev.vp[3] : th;
			float hx = vw ? 1.f / (float)vw : 0.f, hy = vh ? 1.f / (float)vh : 0.f;
			if (!p->shadowValid || p->hpShadow[0] != hx || p->hpShadow[1] != hy) { glUniform2f(p->hp, hx, hy); p->hpShadow[0] = hx; p->hpShadow[1] = hy; }
		}
		if (p->at >= 0)
		{
			float at[4] = {dev.rs[15] ? (float)dev.rs[25] : 0.f, (dev.rs[24] & 255) / 255.f, 0.f, 0.f};
			if (!p->shadowValid || memcmp(at, p->atShadow, 16)) { glUniform4f(p->at, at[0], at[1], 0.f, 0.f); memcpy(p->atShadow, at, 16); }
		}
		p->shadowValid = true;
	}
	uint64_t t3 = profNow();
	bindSamplers(p, ps);
	uint64_t t4 = profNow();
	// vertex attributes
	DeclElem elsBuf[64];
	uint32_t nEls = 0;
	if (dev.vdecl)
	{
		size_t n = std::min<size_t>(dev.vdecl->blob.size() / 8, 64);
		for (size_t i = 0; i < n; i++)
		{
			DeclElem e;
			memcpy(&e, dev.vdecl->blob.data() + 8 * i, 8);
			if (e.stream == 0xff) break;
			elsBuf[nEls++] = e;
		}
	}
	else
	{
		static int ff = 0;
		if (ff++ < 3) log("d3d9: draw with FVF %x and no declaration - not supported yet", dev.fvf);
		return;
	}
	{
		// interface scaling (HUD panels in the corners)
		float axN = 0, ayN = 0, ax = 0, ay = 0;
		static std::vector<uint8_t> split;
		split.clear();
		float k = uiScaleFor(dc, elsBuf, nEls, axN, ayN, ax, ay, split);
		if (!split.empty())
		{
			// pieces in several corners: draw the rebuilt triangles instead (same state, plain triangle list)
			static std::vector<uint8_t> keep;
			keep.swap(split);
			DrawCall d2;
			d2.prim = 4; d2.primCount = dc.primCount; d2.upVerts = keep.data(); d2.upStride = dev.streams[0].stride;
			for (uint32_t q = 0; q < nEls; q++) if (elsBuf[q].usage == 0 && elsBuf[q].index == 0) d2.upStride = dev.streams[elsBuf[q].stream < 16 ? elsBuf[q].stream : 0].stride ? dev.streams[elsBuf[q].stream < 16 ? elsBuf[q].stream : 0].stride : d2.upStride;
			if (dc.upVerts) d2.upStride = dc.upStride;
			gUiNested = true;
			drawImpl(d2);
			gUiNested = false;
			keep.swap(split);
			return;
		}
		if (p->ui >= 0 && (!p->shadowValid || p->uiShadow[0] != axN || p->uiShadow[1] != ayN || p->uiShadow[2] != k))
		{
			glUniform4f(p->ui, axN, ayN, k, 0.f);
			p->uiShadow[0] = axN; p->uiShadow[1] = ayN; p->uiShadow[2] = k;
		}
		if (k != 0.f && dev.rs[174] && dev.scissor[2] > dev.scissor[0])
		{
			// the panel's clip rectangle grows with it
			float s = k + 1.f;
			float x0 = ax + (dev.scissor[0] - ax) * s, y0 = ay + (dev.scissor[1] - ay) * s, x1 = ax + (dev.scissor[2] - ax) * s, y1 = ay + (dev.scissor[3] - ay) * s;
			glScissor((GLint)x0, (GLint)y0, (GLsizei)(x1 - x0 + 0.5f), (GLsizei)(y1 - y0 + 0.5f));
			gStValid = false;   // the next draw sets the game's own scissor again
		}
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
	// only needed when a dynamic vertex stream is copied to the ring (static buffers are used in place):
	// scanning every index of every draw cost tens of microseconds on big terrain/water meshes
	bool needRange = false;
	if (!dc.upVerts)
		for (uint32_t k = 0; k < nEls; k++)
		{
			const Stream& st = dev.streams[elsBuf[k].stream < 16 ? elsBuf[k].stream : 0];
			if (st.vb && (st.vb->usage & 0x200) && !(st.freq & 0x80000000u)) { needRange = true; break; }
		}
	if (needRange && dc.indexed && !dc.upIdx && dev.ib && dev.ib->data && (uint64_t)(dc.startIndex + nvAll) * (dev.ib->fmt == 102 ? 4 : 2) <= dev.ib->size && nvAll)
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
	for (uint32_t k = 0; k < nEls; k++)
	{
		const DeclElem& e = elsBuf[k];
		int loc = shdr::attrLocation(e.usage, e.index);
		if (loc < 0) continue;
		GLint size;
		GLenum gtype;
		bool norm;
		if (!attribFormat(e.type, size, gtype, norm)) continue;
		Stream& st = dev.streams[e.stream < 16 ? e.stream : 0];
		uint32_t stride;
		uintptr_t off = e.offset;
		GLuint attrBuf = gUpVbo;
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
				if (e.type == 4) bgra |= 1u << loc;
				setAttrib(loc, ensureBuffer(st.vb, GL_ARRAY_BUFFER), size, gtype, norm, stride, st.offset + off, 1);
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
				attrBuf = gRingVbo;
				off += (uint64_t)ringBase[si];
			}
			else
			{
				attrBuf = ensureBuffer(st.vb, GL_ARRAY_BUFFER);
				off += st.offset + (int64_t)dc.baseVertex * (int64_t)stride;
			}
		}
		if (e.type == 4) bgra |= 1u << loc;
		setAttrib(loc, attrBuf, size, gtype, norm, stride, off, 0);
		enabled |= 1u << loc;
	}
	disableAttribsExcept(enabled);
	if (p->bgra >= 0 && p->bgraShadow != (GLint)bgra) { glUniform1i(p->bgra, (GLint)bgra); p->bgraShadow = (GLint)bgra; }
	uint64_t t5 = profNow();
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
	gInstUsed = false;   // divisors are part of the cached attribute state (setAttrib)
	if (dc.primCount <= 2 && traceDump() && (!dev.rt0 || isBack(dev.rt0) || dev.rt0->fmt == 21 || dev.rt0->fmt == 22)) dumpTraced("draw", tw, th);
	uint64_t t6 = profNow();
	tProg += t1 - t0; tState += t2 - t1; tUni += t3 - t2; tSamp += t4 - t3; tAttr += t5 - t4; tDraw += t6 - t5; nDraw++;
	gDrawsThisFrame++;
	if (gDrawsThisFrame <= 3 && gFrameNo < 5) glErr("draw");
	if (verbose() && (gFrameNo == 700 || gFrameNo == 1800 || gFrameNo == 3600) && !dev.rt0)
	{
		uint32_t dn = gDrawsThisFrame;
		if (dn == 1 || dn == 10 || dn == 30 || dn == 60 || dn == 90 || dn == 120) dumpBmp(("/sdcard/ra3_f" + std::to_string(gFrameNo) + "_d" + std::to_string(dn) + ".bmp").c_str(), tw, th);
	}
}

static void reapReleased(uint32_t frame);
static void presentImpl();
void present()
{
	rt::waitWhileBackground();   // app in the background: hold the game here (game logic, timers and audio stop)
	uint64_t t0 = nowUs();
	rt::fpsPinTick();
	presentImpl();
	gTPresent += nowUs() - t0;
	static uint64_t start = 0; static uint32_t f0 = 0;
	uint64_t t = nowUs();
	if (!start) { start = t; f0 = gFrameNo; }
	if (gFrameNo - f0 >= 1800)
	{
		double n = gFrameNo - f0, wall = (t - start) / 1000.0;
		uint64_t lim = rt::limiterWaitUs();
		double other = wall - (gTDraw + gTUpload + gTPresent + lim) / 1000.0;
		log("perf: %.1f fps; per frame ms: total %.2f = game/other %.2f + draws %.2f (%.0f calls) + texture uploads %.2f (%.1f) + present %.2f (swap %.2f, waiting for render thread %.2f) + limiter wait %.2f; render thread busy %.2f",
			n / (wall / 1000.0), wall / n, other / n, gTDraw / 1000.0 / n, gNDraws / n, gTUpload / 1000.0 / n, gNUploads / n, gTPresent / 1000.0 / n, gTSwap.load() / 1000.0 / n, gTWaitRender / 1000.0 / n, lim / 1000.0 / n,
			rec::renderBusyUs() / 1000.0 / n);
		start = t; f0 = gFrameNo; gTDraw = gTUpload = gTPresent = gNDraws = gNUploads = gTWaitRender = 0; gTSwap = 0;
	}
}
static void presentImpl()
{
	{ static int n = 0; if (n++ < 5) log("GL: present on tid %d ctx %p", (int)gettid(), (void*)eglGetCurrentContext()); }
	if (!ensureGl()) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); return; }
	if (rec::active()) rec::q([] { gfx::beginFrame(); }); else gfx::beginFrame();
	resizeBackBufferImpl();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	int sw = gfx::width(), sh = gfx::height();
	invalidateGlState();
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
	static GLint blitLoc = glGetUniformLocation(gBlitProg, "t");   // once: a query waits for the render thread
	glUniform1i(blitLoc, 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	// the status/log overlay is only for the very start (nothing rendered yet); once the game has drawn anything, empty frames
	// (loading screens) must not flash it
	static bool everDrew = false;
	if (gDrawsThisFrame > 0) everDrew = true;
	if (gDrawsThisFrame == 0 && !everDrew) { glViewport(0, 0, sw, sh); if (rec::active()) rec::q([] { gfx::drawOverlay(); }); else gfx::drawOverlay(); }
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
	if (rec::active())
	{
		rec::q([] { uint64_t ts = nowUs(); gfx::swap(); gTSwap += nowUs() - ts; });
		uint64_t tw0 = nowUs();
		rec::endFrame();   // keeps at most one frame queued behind the one on the GPU thread
		gTWaitRender += nowUs() - tw0;
		{
			// work this frame = wall time since the last present minus the time the game's frame limiter slept
			static uint64_t lastT = 0, lastLim = 0;
			uint64_t t = nowUs(), lim = rt::limiterWaitTotalUs();
			if (lastT && t > lastT) { uint64_t wall = t - lastT, sl = lim - lastLim; rt::perfHintFrame((wall > sl ? wall - sl : 0) * 1000, rec::renderTid()); }
			lastT = t; lastLim = lim;
		}
	}
	else { uint64_t ts = nowUs(); gfx::swap(); gTSwap += nowUs() - ts; }
	glErr("present");
	gFrameNo++;
	if (gFrameNo == 1)
	{
		// The game's main thread does logic and rendering: keep it on the fast cores and raise its priority.
		unsigned long freq[16] = {0}, maxF = 0; int ncpu = 0;
		for (int i = 0; i < 16; i++)
		{
			char path[96]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
			if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%lu", &freq[i]) == 1) { ncpu = i + 1; maxF = std::max(maxF, freq[i]); } fclose(f); }
		}
		cpu_set_t set; CPU_ZERO(&set); int picked = 0;
		for (int i = 0; i < ncpu; i++) if (freq[i] * 4 >= maxF * 3) { CPU_SET(i, &set); picked++; }
		int ra = picked ? sched_setaffinity(0, sizeof set, &set) : -1;
		int rp = setpriority(PRIO_PROCESS, (id_t)gettid(), -10);
		::profStart();
		log("perf: main thread pinned to %d fast cores of %d (max %lu kHz): affinity %s, priority %s", picked, ncpu, maxF, ra == 0 ? "ok" : "failed", rp == 0 ? "ok" : "failed");
	}
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
	{
		// interface scaling: the panel rectangles of this frame become the touch mapping for the next one
		std::lock_guard<std::mutex> l(gUiMu);
		gUiScale = std::max(1.f, std::min(2.f, rt::config.uiScale));
		gUiPub.swap(gUiCur); gUiCur.clear();
		{
			static int diagFrames = 0;
			bool inGame = gUiWorldDraws >= 50;
			gUiDiag = inGame && gUiScale > 1.001f && diagFrames < 3 && gFrameNo % 900 == 0;
			if (gUiDiag) { diagFrames++; log("ui-diag: frame %u", gFrameNo + 1); }
		}
		// the right column's extents only grow (the open build menu is the largest): its scale stays the same when menus open
		if (gUiHud) { gUiRightT = std::max(gUiRightT, gUiCurT); gUiRightB = std::min(gUiRightB, gUiCurB); if (gUiCurColLeft < 1e8f) gUiColLeft = gUiCurColLeft; }
		gUiCurT = 0.f; gUiCurB = 1.f; gUiCurColLeft = 1e9f;
		gUiWorldDraws = 0; gUiHud = false; gUiAptSeen = false; gUiPieces.clear(); gUiMenu = false;
		static int nl = 0;
		if (gUiScale > 1.001f && !gUiPub.empty() && nl < 3 && gFrameNo > 600) { nl++; log("ui: interface scale %.2f, %zu panel pieces scaled this frame", gUiScale, gUiPub.size()); }
	}
	if (gFrameNo % 1800 == 0) healthReport();
	if (fxStatOn() && gFrameNo % 1800 == 0)
	{
		// effect usage over the last minute: effect#.technique draws/primitives
		std::string o;
		for (int i = 0; i < 512 * 8; i++)
			if (gFxDraws[i]) { char b[48]; snprintf(b, sizeof b, " %d.%d:%u/%u", i / 8, i % 8, gFxDraws[i], gFxPrims[i]); o += b; }
		log("FXSTAT draws/primitives per effect.technique (last 1800 frames):%s", o.c_str());
		memset(gFxDraws, 0, sizeof gFxDraws); memset(gFxPrims, 0, sizeof gFxPrims);
	}
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
	if (!rec::gOn) gfx::release();   // hand the context over: a different game thread may render next
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
		if (o->glTex) { glDeleteTextures(1, &o->glTex); o->glTex = 0; gFboDirty = true; gTexBindDirty = true; }
		if (o->glBuf) { glDeleteBuffers(1, &o->glBuf); o->glBuf = 0; invalidateAttribCache(); }
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
		// gDrawFbo may still carry the depth buffer of the last pass (e.g. the 1024x1024 shadow-map depth): a framebuffer is
		// only as large as its smallest attachment, so the blit would be clipped to that size - detach it
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	}
	invalidateGlState();
	glDisable(GL_SCISSOR_TEST);
	int32_t s[4] = {0, 0, (int32_t)sw, (int32_t)sh}, d[4] = {0, 0, (int32_t)dw, (int32_t)dh};
	if (sr) memcpy(s, sr, 16);
	if (dr) memcpy(d, dr, 16);
	if (traceFrame()) log("TR stretch %s [%d,%d,%d,%d] -> %s [%d,%d,%d,%d] filter %u", isBack(src) ? "back" : objTag(src).c_str(), s[0], s[1], s[2], s[3], isBack(dst) ? "back" : objTag(dst).c_str(), d[0], d[1], d[2], d[3], filter);
	glBlitFramebuffer(s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], GL_COLOR_BUFFER_BIT, filter == 2 ? GL_LINEAR : GL_NEAREST);
	if (traceDump() && (isBack(dst) || dst->fmt == 21 || dst->fmt == 22)) dumpTraced("copy", dw, dh);
	glErr("stretchRect");
}

void destroy(Obj*) {}
void rtSize(Obj* rt, uint32_t& w, uint32_t& h) { targetSize(rt, w, h); }
uint32_t frameNo() { return gFrameNo; }
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
void rtSize(Obj*, uint32_t& w, uint32_t& h) { w = h = 0; }
uint32_t frameNo() { return 0; }
void uiUnmapImpl(int&, int&) {}
void destroy(Obj*) {}
}
}
#endif
namespace rt { void uiUnmap(int& x, int& y) { d3d::glr::uiUnmapImpl(x, y); } }

