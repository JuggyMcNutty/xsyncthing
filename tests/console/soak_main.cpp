// xst-soak.xex: tools/soak.cpp's scan/pull soak on the console's hard disk, with timings and the title's memory.
// Reads the file count from game:/soak.txt (default 10000), works in hdd:/xsyncthing-soak, and logs to xst-soak.log.
#include <cstdio>
#include <fstream>
#include <string>

#include <chrono>
#include <thread>

#include "platform/xsdk/progress_screen.hpp"
#include "xsdk/log.h"

int soak_main(int argc, char **argv);

int main()
{
	// soak.txt: "<files> [bytes per file] [directory] [resume]".
	int n = 10000, bytes = 1024;
	std::string dir = "hdd:/xst-soak", mode;
	std::ifstream in("game:/soak.txt");
	in >> n >> bytes >> dir >> mode;
	std::string a1 = dir, a2 = std::to_string(n), a3 = std::to_string(bytes), a4 = mode;
	char a0[] = "xst-soak";
	char *argv[] = { a0, a1.data(), a2.data(), a3.data(), a4.data(), nullptr };
	// A live progress screen while the soak runs, so a slow phase never looks like a freeze.
	xst::progress::start_screen();
	const int r = soak_main(mode.empty() ? 4 : 5, argv);
	// Leave the result up for a moment before the title ends.
	std::this_thread::sleep_for(std::chrono::seconds(10));
	xst::progress::stop_screen();
	xsdk_log_write("checks: %d passed, %d failed", r == 0, r != 0);
	return r;
}
