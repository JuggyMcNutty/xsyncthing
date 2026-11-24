#pragma once

#include <string>
#include <vector>

// The seam between the portable core and the console. Each backend (src/platform/xsdk, src/platform/posix)
// implements these; the core never calls xsdk directly.
namespace xst::platform {

// Writes one line to the platform's log: the title's log on the console, stderr on the host.
void log(const std::string &line);

// Where xsyncthing keeps its config, keys and database.
std::string data_dir();

// The last few hundred log lines, for /rest/system/log and the console UI's log screen.
struct LogLine {
	std::string when; // RFC 3339
	std::string message;
};
std::vector<LogLine> recent_log();

// /rest/system/restart and /shutdown: the main loop picks this up and acts on it.
enum class RestartKind { none, restart, shutdown };
void request_restart(RestartKind kind);
RestartKind restart_requested();

} // namespace xst::platform
