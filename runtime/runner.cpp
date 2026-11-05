// PC runner: loads the game PE, sets up the main guest thread and calls the PE entry point.
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

#include "launch.h"
#include "rt.h"

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: ra3run <ra3_1.12.game> [gameRoot] [dataRoot]\n"); return 2; }
	if (argc > 2) rt::config.gameRoot = argv[2];
	if (argc > 3) rt::config.dataRoot = argv[3];
	if (const char* lf = getenv("RA3_LOG")) rt::config.logFile = lf;

	if (const char* w = getenv("RA3_WATCHDOG"))
	{
		static int secs = atoi(w);
		std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(secs)); rt::log("watchdog: still running after %d s", secs); rt::traceDump(); _exit(71); }).detach();
	}
	return rt::launch(argv[1]);
}
