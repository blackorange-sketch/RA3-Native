// Android shell: NativeActivity glue, storage permission, game discovery, on-screen log console.
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

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

void draw(ANativeWindow* w)
{
	ANativeWindow_Buffer b;
	if (ANativeWindow_lock(w, &b, nullptr) != 0) return;
	if (b.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM && b.format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM)
	{
		ANativeWindow_unlockAndPost(w);
		return;
	}
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
		ANativeWindow_unlockAndPost(w);
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
	ANativeWindow_unlockAndPost(w);
}

ANativeWindow* window = nullptr;

void startGame(const std::string& game)
{
	std::string dir = game.substr(0, game.rfind('/'));
	std::string leaf = lower(dir.substr(dir.rfind('/') + 1));
	rt::config.gameRoot = leaf == "data" ? dir.substr(0, dir.rfind('/')) : dir;
	pushLine("game: " + game);
	pushLine("game root: " + rt::config.gameRoot);
	status = "running";
	started = true;
	rt::spawnHostThread([game] {
		int code = rt::launch(game);
		rt::log("game exited with code %d", code);
		status = "exited";
	}, 256u << 20);
}

float downY = 0, lastY = 0;
bool dragged = false;

int32_t onInput(android_app*, AInputEvent* ev)
{
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
	if (cmd == APP_CMD_INIT_WINDOW) { window = app->window; ANativeWindow_setBuffersGeometry(window, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM); dirty = true; }
	if (cmd == APP_CMD_TERM_WINDOW) window = nullptr;
}
}

void android_main(android_app* app)
{
	app->onAppCmd = onCmd;
	app->onInputEvent = onInput;
	savedFile = std::string(app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp") + "/gamepath.txt";
	rt::logHook = logSink;
	rt::config.dataRoot = app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp";
	rt::config.logFile = "/sdcard/ra3_native.log";
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
		if (window && dirty.exchange(false)) draw(window);
	}
}
