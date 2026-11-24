// xst-soak.xex: tools/soak.cpp's scan/pull soak on the console's hard disk, with timings and the title's memory.
// Reads the file count from game:/soak.txt (default 10000), works in hdd:/xsyncthing-soak, and logs to xst-soak.log.
#include <cstdio>
#include <fstream>
#include <string>

#include "xsdk/log.h"

int soak_main(int argc, char **argv);

int main()
{
	// soak.txt: "<files> [bytes per file] [directory]".
	int n = 10000, bytes = 1024;
	std::string dir = "hdd:/xst-soak";
	std::ifstream in("game:/soak.txt");
	in >> n >> bytes >> dir;
	std::string a1 = dir, a2 = std::to_string(n), a3 = std::to_string(bytes);
	char a0[] = "xst-soak";
	char *argv[] = { a0, a1.data(), a2.data(), a3.data(), nullptr };
	const int r = soak_main(4, argv);
	xsdk_log_write("checks: %d passed, %d failed", r == 0, r != 0);
	return r;
}
