#pragma once

// Live progress for long-running work (soak and test titles, and the daemon's own scans and pulls): the phase, a
// count with rate and time left, and the last log lines. The work thread reports; on the console a drawing thread
// shows it at 720p (src/platform/xsdk/progress_screen.cpp), so a slow run never looks like a freeze. On the host,
// changes are printed as lines.
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace xst::progress {

struct State {
	std::string title;      // "xsyncthing soak"
	std::string phase;      // "pulling"
	int64_t done = 0, total = 0;
	std::string unit = "files";
	std::chrono::steady_clock::time_point phase_started{}, started{};
	std::vector<std::string> notes; // finished phases' results, newest last
	bool finished = false;
	bool failed = false;
};

void set_title(const std::string &title);
// Starts a phase; total 0 means unknown.
void phase(const std::string &name, int64_t total = 0, const std::string &unit = "files");
void advance(int64_t n = 1);
void set_done(int64_t done);
// A finished phase's one-line result, kept on screen.
void note(const std::string &line);
void finish(bool ok);
State snapshot();

// "12.3 files/s, about 4 min left" for the current phase, or "" if there's nothing to estimate yet.
std::string rate_text(const State &s);
std::string elapsed_text(std::chrono::steady_clock::duration d);

} // namespace xst::progress
