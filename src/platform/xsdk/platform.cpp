#include "platform/platform.hpp"

#include "xsdk/log.h"

namespace xst::platform {

void log(const std::string &line) { xsdk_log_write("%s", line.c_str()); }

// The internal hard disk, which every console with homebrew has.
std::string data_dir() { return "hdd:/xsyncthing"; }

} // namespace xst::platform
