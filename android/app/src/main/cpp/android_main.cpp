#include <chrono>
// Android shell: NativeActivity glue, storage permission, game discovery, on-screen log console.
#include "input.h"
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <vector>
#include "gfx.h"
#include <android/hardware_buffer.h>
#include <android_native_app_glue.h>
#include <android/window.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "font8x16.h"
#include "launch.h"
#include "hud.h"

namespace
{
std::mutex lineMu;
std::deque<std::string> lines;
std::atomic<bool> dirty{true}, started{false};
std::string status = "starting";

void pushLine(const std::string& s)
{
	std::lock_guard<std::mutex> l(lineMu);
	lines.push_back(s);
	while (lines.size() > 400) lines.pop_front();
	dirty = true;
}

void logSink(const char* s)
{
	__android_log_print(ANDROID_LOG_INFO, "RA3", "%s", s);
	pushLine(s);
}

bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Looks for ra3_1.12.game (case-insensitive) up to three directory levels below the storage roots.
std::string findGame()
{
	std::vector<std::pair<std::string, int>> q = {{"/sdcard", 0}};
	size_t visited = 0;
	for (size_t i = 0; i < q.size() && visited < 30000; i++, visited++)
	{
		DIR* d = opendir(q[i].first.c_str());
		if (!d) continue;
		while (auto* e = readdir(d))
		{
			std::string n = e->d_name;
			if (n == "." || n == "..") continue;
			std::string full = q[i].first + "/" + n;
			if (lower(n) == "ra3_1.12.game") { closedir(d); return full; }
			if (e->d_type == DT_DIR && q[i].second < 3 && lower(n) != "android" && n[0] != '.') q.push_back({full, q[i].second + 1});
		}
		closedir(d);
	}
	return "";
}


// ---- in-app folder picker (NativeActivity has no Java side, so the system picker cannot return a result)
struct PickEntry { std::string label, path; int kind; };   // kind: 0 dir, 1 up, 2 use
bool picking = false;
std::string pickDir = "/sdcard", savedFile;
std::vector<PickEntry> pickEntries;
int pickScroll = 0, pickCh = 32;

// ra3_1.12.game directly inside dir, or inside dir/Data (case-insensitive); empty if none
std::string gameIn(const std::string& dir)
{
	for (const std::string& base : {dir, dir + "/Data", dir + "/data", dir + "/DATA"})
		if (DIR* d = opendir(base.c_str()))
		{
			while (auto* e = readdir(d))
				if (lower(e->d_name) == "ra3_1.12.game") { std::string r = base + "/" + e->d_name; closedir(d); return r; }
			closedir(d);
		}
	return "";
}

void rebuildPick()
{
	pickEntries.clear();
	pickScroll = 0;
	if (!gameIn(pickDir).empty()) pickEntries.push_back({"[ USE THIS FOLDER ]", pickDir, 2});
	if (pickDir != "/" && pickDir != "/sdcard") pickEntries.push_back({"[ .. up ]", pickDir.substr(0, pickDir.rfind('/')), 1});
	std::vector<std::string> names;
	if (DIR* d = opendir(pickDir.c_str()))
	{
		while (auto* e = readdir(d))
		{
			std::string n = e->d_name;
			if (n[0] == '.') continue;
			struct stat st;
			if (stat((pickDir + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) names.push_back(n);
		}
		closedir(d);
	}
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
	for (auto& n : names) pickEntries.push_back({n + "/", pickDir + "/" + n, 0});
	dirty = true;
}

void startPicking()
{
	picking = true;
	if (!exists(pickDir)) pickDir = "/sdcard";
	rebuildPick();
}


// Soft keyboard: MainActivity (Java) owns an invisible EditText and forwards typed characters through these natives.
void jniChar(JNIEnv*, jclass, jint cu) { input::onChar((unsigned)cu); }
void jniKey(JNIEnv*, jclass, jint k, jboolean down) { input::onKey(down, k, 0); }

void registerNatives(android_app* app)
{
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);
	jclass cls = env->GetObjectClass(app->activity->clazz);
	static const JNINativeMethod m[] = {{"nativeChar", "(I)V", (void*)jniChar}, {"nativeKey", "(IZ)V", (void*)jniKey}};
	if (env->RegisterNatives(cls, m, 2) != 0) { env->ExceptionClear(); rt::log("keyboard: RegisterNatives failed"); }
	app->activity->vm->DetachCurrentThread();
}

void toggleKeyboard(android_app* app)
{
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);
	jclass cls = env->GetObjectClass(app->activity->clazz);
	jmethodID mid = env->GetMethodID(cls, "toggleKeyboard", "()V");
	if (mid) env->CallVoidMethod(app->activity->clazz, mid);
	if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); rt::log("keyboard: toggle failed"); }
	app->activity->vm->DetachCurrentThread();
}

bool hasAllFilesAccess(android_app* app)
{
	if (android_get_device_api_level() < 30) return true;
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);
	jclass cls = env->FindClass("android/os/Environment");
	jmethodID m = env->GetStaticMethodID(cls, "isExternalStorageManager", "()Z");
	bool ok = env->CallStaticBooleanMethod(cls, m);
	app->activity->vm->DetachCurrentThread();
	return ok;
}

void requestAllFilesAccess(android_app* app)
{
	JNIEnv* env = nullptr;
	app->activity->vm->AttachCurrentThread(&env, nullptr);
	jclass uriCls = env->FindClass("android/net/Uri");
	jmethodID parse = env->GetStaticMethodID(uriCls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
	jobject uri = env->CallStaticObjectMethod(uriCls, parse, env->NewStringUTF("package:org.ra3native"));
	jclass intentCls = env->FindClass("android/content/Intent");
	jmethodID ctor = env->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
	jobject intent = env->NewObject(intentCls, ctor, env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
	jclass actCls = env->GetObjectClass(app->activity->clazz);
	jmethodID start = env->GetMethodID(actCls, "startActivity", "(Landroid/content/Intent;)V");
	env->CallVoidMethod(app->activity->clazz, start, intent);
	if (env->ExceptionCheck()) env->ExceptionClear();
	app->activity->vm->DetachCurrentThread();
}

bool quietStart = false;   // launched from the launcher: loading screen instead of the log console
static uint64_t rtNowMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void draw(ANativeWindow* w)
{
	if (!gfx::ready()) return;
	struct { int width, height, stride; uint32_t* bits; } b;
	b.width = gfx::width(); b.height = gfx::height(); b.stride = b.width;
	if (b.width <= 0 || b.height <= 0) return;
	static std::vector<uint32_t> cpuBuf;
	cpuBuf.assign((size_t)b.width * b.height, 0);
	b.bits = cpuBuf.data();
	auto unlockAndPost = [&] { gfx::postOverlay(cpuBuf.data(), b.width, b.height); if (!gfx::ownedByGame()) gfx::drawOverlayAndPresent(); };
	uint32_t* px = (uint32_t*)b.bits;
	for (int y = 0; y < b.height; y++)
		for (int x = 0; x < b.width; x++) px[y * b.stride + x] = 0xff101418;
	const int scale = picking ? 3 : (b.width >= 1600 ? 2 : 1), cw = 8 * scale, ch = 16 * scale;
	pickCh = ch;
	int cols = b.width / cw - 1, rows = b.height / ch - 2;
	auto text = [&](int row, const std::string& s, uint32_t color) {
		for (int i = 0; i < (int)s.size() && i < cols; i++)
		{
			unsigned char c = s[i];
			if (c < 32 || c > 126) c = '?';
			for (int gy = 0; gy < 16; gy++)
				for (int gx = 0; gx < 8; gx++)
					if (kFont8x16[c - 32][gy] >> (7 - gx) & 1)
						for (int sy = 0; sy < scale; sy++)
							for (int sx = 0; sx < scale; sx++)
							{
								int X = (i + 1) * cw + gx * scale + sx, Y = (row + 1) * ch + gy * scale + sy;
								if (X < b.width && Y < b.height) px[Y * b.stride + X] = color;
							}
		}
	};
	if (picking)
	{
		text(0, "Game folder: " + pickDir, 0xff7fd4ff);
		for (int r = 0; r < rows - 1; r++)
		{
			int i = pickScroll + r;
			if (i >= (int)pickEntries.size()) break;
			text(r + 1, pickEntries[i].label, pickEntries[i].kind == 0 ? 0xffd8d8d8 : 0xff80ff80);
		}
		unlockAndPost();
		return;
	}
	bool fatalShown = false;
	{
		std::lock_guard<std::mutex> l(lineMu);
		for (auto& s : lines) if (s.rfind("FATAL", 0) == 0) fatalShown = true;
	}
	if (quietStart && !fatalShown && !picking)
	{
		// started from the launcher: the launcher's backdrop with a progress bar (the console returns on errors)
		const int W = b.width, H = b.height, dp = std::max(1, H / 400);
		static std::vector<uint32_t> bg;
		static int bgW = 0, bgH = 0;
		if (bgW != W || bgH != H)
		{
			// vertical dark gradient, a red glow at the upper left third, faint scan lines (as LauncherActivity.Backdrop)
			bg.assign((size_t)W * H, 0);
			float gx = W * 0.28f, gy = H * 0.35f, gr = std::max(W, H) * 0.6f;
			for (int y = 0; y < H; y++)
			{
				float t = (float)y / std::max(1, H - 1);
				float br = 0x0c + (0x05 - 0x0c) * t, bgc = 0x0d + (0x05 - 0x0d) * t, bb = 0x0f + (0x05 - 0x0f) * t;
				bool scan = (y % (3 * dp)) == 0;
				for (int x = 0; x < W; x++)
				{
					float d = std::sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy)) / gr;
					float a = d < 1.f ? 0.4f * (1.f - d) : 0.f;   // 0x66 alpha fading out
					float r = br + (0xa0 - br) * a, g = bgc + (0x0d - bgc) * a, bl = bb + (0x10 - bb) * a;
					if (scan) { r += (255 - r) * 0.047f; g += (255 - g) * 0.047f; bl += (255 - bl) * 0.047f; }
					bg[(size_t)y * W + x] = 0xff000000u | ((uint32_t)bl << 16) | ((uint32_t)g << 8) | (uint32_t)r;   // memory order R, G, B, A (0xAABBGGRR)
				}
			}
			bgW = W; bgH = H;
		}
		memcpy(px, bg.data(), bg.size() * 4);
		auto big = [&](int x0, int y0, int sc, const std::string& str, uint32_t col) {
			for (size_t i = 0; i < str.size(); i++)
			{
				unsigned char c = str[i];
				if (c < 32 || c > 126) c = '?';
				for (int gy = 0; gy < 16; gy++)
					for (int gx = 0; gx < 8; gx++)
						if (kFont8x16[c - 32][gy] >> (7 - gx) & 1)
							for (int sy = 0; sy < sc; sy++)
								for (int sx = 0; sx < sc; sx++)
								{
									int X = x0 + ((int)i * 8 + gx) * sc + sx, Y = y0 + gy * sc + sy;
									if (X >= 0 && X < W && Y >= 0 && Y < H) px[Y * b.stride + X] = col;
								}
			}
		};
		// progress: bytes the game has read so far against what the last start needed before its first frame
		uint64_t rb = 0, ru = 0, rc = 0;
		rt::ioStats(rb, ru, rc);
		static uint64_t expect = 0;
		if (!expect)
		{
			expect = 400ull << 20;   // first start: a guess
			if (FILE* f = fopen((rt::config.dataRoot + "/loadbytes").c_str(), "r")) { unsigned long long v = 0; if (fscanf(f, "%llu", &v) == 1 && v > (16ull << 20)) expect = v; fclose(f); }
		}
		float prog = started ? std::min(0.97f, (float)rb / (float)expect) : 0.f;
		static float shown = 0.f;
		shown += (prog - shown) * 0.25f;
		// title and status
		int sc = std::max(2, H / 180);
		std::string title = "RED ALERT 3";
		big((W - (int)title.size() * 8 * sc) / 2, H / 2 - 16 * sc, sc, title, 0xffffffff);
		int sc2 = std::max(1, sc / 2);
		char st[64];
		snprintf(st, sizeof st, "LOADING  %d%%", (int)(shown * 100.f + 0.5f));
		big((W - (int)strlen(st) * 8 * sc2) / 2, H / 2 + 4 * sc, sc2, st, 0xff1ac2ff);
		// red rule + hazard stripes as the progress bar (lit part = progress), like the launcher
		int bh = 8 * dp;
		for (int y = H - bh - 3 * dp; y < H - bh; y++)
			for (int x = 0; x < W; x++) px[y * b.stride + x] = 0xff0c0a6e;   // RED_DARK
		int fill = (int)(W * shown);
		for (int y = H - bh; y < H; y++)
			for (int x = 0; x < W; x++)
			{
				int d = (x + (H - y)) % (bh * 2);
				bool stripe = d < bh;
				uint32_t c = 0xff111111;
				if (stripe) c = x <= fill ? 0xff1ac2ffu : 0xff163c4au;   // YELLOW lit / dim
				px[y * b.stride + x] = c;
			}
		for (int y = 6 * dp; y < 7 * dp; y++)
			for (int x = 0; x < W; x++) px[y * b.stride + x] = 0xff1a1680;   // thin top rule
		unlockAndPost();
		return;
	}
	text(0, "RA3 Native - " + status + "  (tap here to choose game folder)", 0xff7fd4ff);
	std::lock_guard<std::mutex> l(lineMu);
	int start = (int)lines.size() > rows ? (int)lines.size() - rows : 0;
	for (int r = 0; start + r < (int)lines.size(); r++)
	{
		const std::string& s = lines[start + r];
		text(r + 1, s, s.rfind("FATAL", 0) == 0 ? 0xffff6060 : 0xffd8d8d8);
	}
	unlockAndPost();
}

ANativeWindow* window = nullptr;


// Wide screen (launcher option): RA3 widens its view (Hor+) for any resolution set in a profile's Options.ini, but its
// menu only offers standard aspect ratios. Each start, the Resolution of every profile is set to the screen's aspect
// ratio minus the button columns (same height); with the option off a wide value goes back to 16:9.
static void patchProfileResolutions()
{
	int sw = gfx::width(), sh = gfx::height();
	if (sw <= 0 || sh <= 0) { if (window) { sw = ANativeWindow_getWidth(window); sh = ANativeWindow_getHeight(window); } }
	if (sw <= 0 || sh <= 0) return;
	if (sh > sw) std::swap(sw, sh);
	int rl = 0, rr = 0;
	hud::reserves(sw, sh, rl, rr);
	double wide = (double)(sw - rl - rr) / sh;
	std::string root = rt::config.dataRoot + "/users/player/documents/red alert 3/profiles";
	DIR* d = opendir(root.c_str());
	if (!d) return;
	while (dirent* e = readdir(d))
	{
		if (e->d_name[0] == '.') continue;
		std::string dir = root + "/" + e->d_name;
		DIR* pd = opendir(dir.c_str());
		if (!pd) continue;
		std::string file;
		while (dirent* f = readdir(pd)) if (strcasecmp(f->d_name, "options.ini") == 0) file = dir + "/" + f->d_name;
		closedir(pd);
		if (file.empty()) continue;
		FILE* in = fopen(file.c_str(), "rb");
		if (!in) continue;
		std::string text, out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, in)) > 0) text.append(buf, n);
		fclose(in);
		bool changed = false;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
			pos = eol == std::string::npos ? text.size() : eol + 1;
			int w = 0, h = 0;
			char key[16] = {};
			if (sscanf(line.c_str(), " %15[A-Za-z] = %d %d", key, &w, &h) == 3 && strcasecmp(key, "Resolution") == 0 && w > 0 && h > 0)
			{
				double a = rt::config.wideScreen ? wide : 16.0 / 9.0;
				int nw = ((int)(h * a + 0.5)) & ~1;
				bool wider = (double)w / h > 16.0 / 9.0 + 0.01;
				if ((rt::config.wideScreen && nw != w && nw > w * 1.0) || (!rt::config.wideScreen && wider))
				{
					rt::log("wide screen: %s: Resolution %d %d -> %d %d", e->d_name, w, h, nw, h);
					std::string cr = !line.empty() && line.back() == '\r' ? "\r" : "";
					line = "Resolution = " + std::to_string(nw) + " " + std::to_string(h) + cr;
					changed = true;
				}
			}
			out += line;
			if (eol != std::string::npos) out += '\n';
		}
		if (changed) if (FILE* o = fopen(file.c_str(), "wb")) { fwrite(out.data(), 1, out.size(), o); fclose(o); }
	}
	closedir(d);
}

void startGame(const std::string& game)
{
	std::string dir = game.substr(0, game.rfind('/'));
	std::string leaf = lower(dir.substr(dir.rfind('/') + 1));
	rt::config.gameRoot = leaf == "data" ? dir.substr(0, dir.rfind('/')) : dir;
	pushLine("game: " + game);
	pushLine("game root: " + rt::config.gameRoot);
	rt::log("launcher: language %s, cutscenes %d, log %d, fast shaders %d, wide screen %d, buttons %d%% / opacity %d%%, drag the map %d", rt::config.language.c_str(), (int)rt::config.playMovies, (int)!rt::config.logFile.empty(), (int)rt::config.fastShaders, (int)rt::config.wideScreen, rt::config.buttonSize, rt::config.buttonOpacity, (int)rt::config.panInvert);
	patchProfileResolutions();
	status = "running";
	started = true;
	rt::spawnHostThread([game] {
		int code = rt::launch(game);
		rt::log("game exited with code %d", code);
		status = "exited";
	}, 256u << 20);
}

android_app* gApp = nullptr;
float downY = 0, lastY = 0;
bool dragged = false;

int32_t onInput(android_app*, AInputEvent* ev)
{
	if (started && !picking)
	{
		// the game is running: touches and keys become Win32 mouse / keyboard messages
		if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY)
		{
			int k = AKeyEvent_getKeyCode(ev), a = AKeyEvent_getAction(ev);
			if (k == AKEYCODE_VOLUME_UP || k == AKEYCODE_VOLUME_DOWN || k == AKEYCODE_VOLUME_MUTE) return 0;   // let the system change the volume
			if (a == AKEY_EVENT_ACTION_DOWN || a == AKEY_EVENT_ACTION_UP) input::onKey(a == AKEY_EVENT_ACTION_DOWN, k, AKeyEvent_getMetaState(ev));
			return 1;
		}
		if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION)
		{
			int32_t raw = AMotionEvent_getAction(ev);
			int act = raw & AMOTION_EVENT_ACTION_MASK, idx = (raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
			int n = (int)std::min<size_t>(AMotionEvent_getPointerCount(ev), 5);
			float xs[5], ys[5];
			for (int i = 0; i < n; i++) { xs[i] = AMotionEvent_getX(ev, i); ys[i] = AMotionEvent_getY(ev, i); }
			input::onTouch(act, idx, n, xs, ys);
			if (input::takeKeyboardToggle()) { rt::log("keyboard: button tapped"); toggleKeyboard(gApp); }
			return 1;
		}
		return 0;
	}
	if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
	int act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
	float y = AMotionEvent_getY(ev, 0);
	if (act == AMOTION_EVENT_ACTION_DOWN) { downY = lastY = y; dragged = false; }
	else if (act == AMOTION_EVENT_ACTION_MOVE && picking)
	{
		if (fabsf(y - downY) > pickCh / 2) dragged = true;
		int rowsMoved = (int)((lastY - y) / pickCh);
		if (rowsMoved != 0)
		{
			pickScroll = std::max(0, std::min(pickScroll + rowsMoved, std::max(0, (int)pickEntries.size() - 1)));
			lastY = y;
			dirty = true;
		}
	}
	else if (act == AMOTION_EVENT_ACTION_UP && !dragged)
	{
		int line = (int)(y / pickCh) - 1;   // line 0 is the header
		if (!picking)
		{
			if (line < 0) startPicking();
		}
		else if (line >= 0)
		{
			int i = pickScroll + line;
			if (i < (int)pickEntries.size())
			{
				PickEntry e = pickEntries[i];
				if (e.kind == 2)
				{
					std::string game = gameIn(e.path);
					if (FILE* f = fopen(savedFile.c_str(), "w")) { fputs(game.c_str(), f); fclose(f); }
					picking = false;
					dirty = true;
					if (started) pushLine("Folder saved: " + game + " - restart the app to use it.");
					else startGame(game);
				}
				else { pickDir = e.path; rebuildPick(); }
			}
		}
		else rebuildPick();
	}
	return 1;
}

void onCmd(android_app* app, int32_t cmd)
{
	if (cmd == APP_CMD_INIT_WINDOW) { window = app->window; gfx::setWindow(window); dirty = true; }
	if (cmd == APP_CMD_TERM_WINDOW) { gfx::setWindow(nullptr); window = nullptr; }
	// another app in front / home / screen off: pause the game until it is visible again
	if (cmd == APP_CMD_PAUSE && started) rt::setBackground(true);
	if (cmd == APP_CMD_RESUME) rt::setBackground(false);
}
}

void android_main(android_app* app)
{
	gApp = app;
	registerNatives(app);
	app->onAppCmd = onCmd;
	app->onInputEvent = onInput;
	ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);   // no screen timeout while playing
	savedFile = std::string(app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp") + "/gamepath.txt";
	rt::logHook = logSink;
	rt::config.dataRoot = app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp";
	rt::config.logFile = "/sdcard/ra3_native.log";
	// settings chosen in the launcher (LauncherActivity writes <app data>/launcher.cfg as key=value lines)
	if (FILE* f = fopen((rt::config.dataRoot + "/launcher.cfg").c_str(), "r"))
	{
		quietStart = true;
		char ln[512];
		while (fgets(ln, sizeof ln, f))
		{
			std::string l = ln;
			while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
			size_t eq = l.find('=');
			if (eq == std::string::npos) continue;
			std::string k = l.substr(0, eq), v = l.substr(eq + 1);
			if (k == "language" && !v.empty() && v.find_first_of("/\\ \"") == std::string::npos) rt::config.language = v;
			else if (k == "cutscenes") rt::config.playMovies = v == "1";
			else if (k == "log") { if (v != "1") rt::config.logFile.clear(); }
			else if (k == "fps60") rt::config.fps60 = v == "1";
			else if (k == "fast_shaders") rt::config.fastShaders = v == "1";
			else if (k == "wide_screen") rt::config.wideScreen = v == "1";
			else if (k == "button_size") { int pc = atoi(v.c_str()); if (pc >= 50 && pc <= 300) rt::config.buttonSize = pc; }
			else if (k == "button_opacity") { int pc = atoi(v.c_str()); if (pc >= 10 && pc <= 100) rt::config.buttonOpacity = pc; }
			else if (k == "pan_invert") rt::config.panInvert = v == "1";
			else if (k == "ui_scale") { int pc = atoi(v.c_str()); if (pc >= 100 && pc <= 200) rt::config.uiScale = pc / 100.f; }
			else if (k == "gamepath" && !v.empty())
			{
				// the launcher found or was given the game: remember it like the in-app picker does
				if (FILE* g = fopen(savedFile.c_str(), "w")) { fputs(v.c_str(), g); fclose(g); }
			}
		}
		fclose(f);
	}
	bool asked = false;
	int tick = 0;

	while (!app->destroyRequested)
	{
		int ev;
		android_poll_source* src;
		while (ALooper_pollOnce(100, nullptr, &ev, (void**)&src) >= 0)
			if (src) src->process(app, src);

		if (!started && window)
		{
			if (!hasAllFilesAccess(app))
			{
				status = "waiting for 'All files access' permission";
				if (!asked) { asked = true; pushLine("Grant 'All files access' to RA3 Native in the Settings screen, then return here."); requestAllFilesAccess(app); }
			}
			else if (!picking && tick % 10 == 0)
			{
				std::string game;
				if (FILE* f = fopen(savedFile.c_str(), "r")) { char buf[1024] = {}; if (fgets(buf, sizeof buf, f)) game = buf; fclose(f); }
				if (game.empty() || !exists(game)) game = findGame();
				if (game.empty())
				{
					status = "choose the game folder";
					pushLine("ra3_1.12.game not found automatically - pick the game folder.");
					startPicking();
				}
				else startGame(game);
			}
		}
		tick++;
		gfx::servicePlatform();
		if (window && (dirty.exchange(false) || (quietStart && !gfx::ownedByGame()))) draw(window);
		{
			// remember how much the game read before its first frame: the next loading screen's 100%
			static bool saved = false;
			if (!saved && started && gfx::ownedByGame())
			{
				saved = true;
				uint64_t rb = 0, ru = 0, rc = 0;
				rt::ioStats(rb, ru, rc);
				if (rb > (16ull << 20)) if (FILE* f = fopen((rt::config.dataRoot + "/loadbytes").c_str(), "w")) { fprintf(f, "%llu\n", (unsigned long long)rb); fclose(f); }
			}
		}
	}
	// The activity is gone (closed from the recent apps list, or finished by the system). The game's threads would
	// otherwise keep running in a process without a window, using CPU and battery for hours: end the process.
	if (started)
	{
		rt::log("activity destroyed: exiting");
		_exit(0);
	}
}
