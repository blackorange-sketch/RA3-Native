// Android shell: NativeActivity glue, storage permission, game discovery, on-screen log console.
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
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
	uint32_t* px = (uint32_t*)b.bits;
	for (int y = 0; y < b.height; y++)
		for (int x = 0; x < b.width; x++) px[y * b.stride + x] = 0xff101418;
	const int scale = b.width >= 1600 ? 2 : 1, cw = 8 * scale, ch = 16 * scale;
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
	text(0, "RA3 Native - " + status, 0xff7fd4ff);
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

void onCmd(android_app* app, int32_t cmd)
{
	if (cmd == APP_CMD_INIT_WINDOW) { window = app->window; dirty = true; }
	if (cmd == APP_CMD_TERM_WINDOW) window = nullptr;
}
}

void android_main(android_app* app)
{
	app->onAppCmd = onCmd;
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
			else if (tick % 10 == 0)
			{
				std::string game = findGame();
				if (game.empty()) { status = "ra3_1.12.game not found"; pushLine("Cannot find ra3_1.12.game under /sdcard (looked 3 levels deep). Copy the game folder to the phone storage."); }
				else
				{
					std::string dir = game.substr(0, game.rfind('/'));
					std::string leaf = lower(dir.substr(dir.rfind('/') + 1));
					rt::config.gameRoot = leaf == "data" ? dir.substr(0, dir.rfind('/')) : dir;
					pushLine("game: " + game);
					pushLine("game root: " + rt::config.gameRoot);
					status = "running";
					started = true;
					std::thread([game] {
						int code = rt::launch(game);
						rt::log("game exited with code %d", code);
						status = "exited";
					}).detach();
				}
			}
		}
		tick++;
		if (window && dirty.exchange(false)) draw(window);
	}
}
