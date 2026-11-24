// The parts of the platform layer both backends share: the in-memory log ring and the restart request.
#include <atomic>
#include <deque>
#include <mutex>

#include "core/events.hpp"
#include "platform/platform.hpp"

namespace xst::platform {

namespace {
std::mutex g_mu;
std::deque<LogLine> g_lines;
std::atomic<int> g_restart{ 0 };
} // namespace

void remember_log(const std::string &line)
{
	std::lock_guard lock(g_mu);
	g_lines.push_back({ rfc3339_now(), line });
	while (g_lines.size() > 500)
		g_lines.pop_front();
}

std::vector<LogLine> recent_log()
{
	std::lock_guard lock(g_mu);
	return std::vector<LogLine>(g_lines.begin(), g_lines.end());
}

void request_restart(RestartKind kind) { g_restart = static_cast<int>(kind); }
RestartKind restart_requested() { return static_cast<RestartKind>(g_restart.load()); }

} // namespace xst::platform
