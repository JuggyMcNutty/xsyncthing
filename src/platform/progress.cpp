#include "platform/progress.hpp"

#include <cstdio>

namespace xst::progress {

namespace {
std::mutex g_mu;
State g_state;
} // namespace

void set_title(const std::string &title)
{
	std::lock_guard lock(g_mu);
	g_state.title = title;
	if (g_state.started == std::chrono::steady_clock::time_point{})
		g_state.started = std::chrono::steady_clock::now();
}

void phase(const std::string &name, int64_t total, const std::string &unit)
{
	std::lock_guard lock(g_mu);
	g_state.phase = name;
	g_state.done = 0;
	g_state.total = total;
	g_state.unit = unit;
	g_state.phase_started = std::chrono::steady_clock::now();
	if (g_state.started == std::chrono::steady_clock::time_point{})
		g_state.started = g_state.phase_started;
}

void advance(int64_t n)
{
	std::lock_guard lock(g_mu);
	g_state.done += n;
}

void set_done(int64_t done)
{
	std::lock_guard lock(g_mu);
	g_state.done = done;
}

void note(const std::string &line)
{
	std::lock_guard lock(g_mu);
	g_state.notes.push_back(line);
	if (g_state.notes.size() > 12)
		g_state.notes.erase(g_state.notes.begin());
}

void finish(bool ok)
{
	std::lock_guard lock(g_mu);
	g_state.finished = true;
	g_state.failed = !ok;
	g_state.phase = ok ? "finished" : "finished with failures";
}

State snapshot()
{
	std::lock_guard lock(g_mu);
	return g_state;
}

std::string elapsed_text(std::chrono::steady_clock::duration d)
{
	const auto s = std::chrono::duration_cast<std::chrono::seconds>(d).count();
	char buf[32];
	if (s >= 3600)
		std::snprintf(buf, sizeof buf, "%lldh %02lldm", (long long)(s / 3600), (long long)(s / 60 % 60));
	else
		std::snprintf(buf, sizeof buf, "%lldm %02llds", (long long)(s / 60), (long long)(s % 60));
	return buf;
}

std::string rate_text(const State &s)
{
	const double secs =
		std::chrono::duration<double>(std::chrono::steady_clock::now() - s.phase_started).count();
	if (s.done <= 0 || secs < 1)
		return "";
	const double rate = s.done / secs;
	char buf[96];
	if (s.total > s.done && rate > 0) {
		const auto left = std::chrono::seconds(static_cast<int64_t>((s.total - s.done) / rate));
		std::snprintf(buf, sizeof buf, "%.1f %s/s, about %s left", rate, s.unit.c_str(),
			elapsed_text(left).c_str());
	} else {
		std::snprintf(buf, sizeof buf, "%.1f %s/s", rate, s.unit.c_str());
	}
	return buf;
}

} // namespace xst::progress
