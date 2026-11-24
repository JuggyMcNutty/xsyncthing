#include "platform/platform.hpp"

#include <cstdio>
#include <cstdlib>

namespace xst::platform {

void log(const std::string &line) { std::fprintf(stderr, "%s\n", line.c_str()); }

std::string data_dir()
{
	if (const char *dir = std::getenv("XSYNCTHING_HOME"))
		return dir;
	if (const char *home = std::getenv("HOME"))
		return std::string(home) + "/.local/state/xsyncthing";
	return ".";
}

} // namespace xst::platform
