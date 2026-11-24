#pragma once

#include <string>

// The seam between the portable core and the console. Each backend (src/platform/xsdk, src/platform/posix)
// implements these; the core never calls xsdk directly.
namespace xst::platform {

// Writes one line to the platform's log: the title's log on the console, stderr on the host.
void log(const std::string &line);

// Where xsyncthing keeps its config, keys and database.
std::string data_dir();

} // namespace xst::platform
