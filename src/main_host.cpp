// The host build: runs the portable core on Linux, for development and interop tests.
#include <format>

#include "core/version.hpp"
#include "platform/platform.hpp"

int main()
{
	xst::platform::log(std::format("{} {} (host)", xst::client_name(), xst::client_version()));
	xst::platform::log("data dir: " + xst::platform::data_dir());
	return 0;
}
