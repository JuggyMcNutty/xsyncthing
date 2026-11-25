#include "platform/xsdk/progress_screen.hpp"

#include <atomic>
#include <cstdio>
#include <format>
#include <string>
#include <thread>

#include "platform/platform.hpp"
#include "platform/progress.hpp"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/text.h"

namespace xst::progress {

namespace {

constexpr uint32_t kClear = 0xFF0B1620;
constexpr uint32_t kWhite = 0xFFFFFFFF;
constexpr uint32_t kDim = 0xFF9FB4C0;
constexpr uint32_t kAccent = 0xFF0088D1;
constexpr uint32_t kGood = 0xFF2ECC71;
constexpr uint32_t kBad = 0xFFE74C3C;
constexpr int32_t kX = 128, kY = 72; // title-safe area of 1280x720

std::atomic<bool> g_stop{ false };
std::atomic<bool> g_ready{ false };
std::thread g_thread;

// A progress bar of text cells: full blocks and light shade (the font has both).
std::string bar(double frac, int cells)
{
	const int full = static_cast<int>(frac * cells + 0.5);
	std::string s;
	for (int i = 0; i < cells; i++)
		s += i < full ? "\u2588" : "\u2591";
	return s;
}

void draw_loop()
{
	if (!NT_SUCCESS(xsdk_gpu_open())) {
		g_ready = true;
		return;
	}
	uint32_t w = 0, h = 0;
	if (!NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &w, &h)) || !NT_SUCCESS(xsdk_text_open())) {
		xsdk_gpu_close();
		g_ready = true;
		return;
	}
	g_ready = true;
	uint32_t frame = 0;
	while (!g_stop) {
		uint32_t target = 0;
		if (!NT_SUCCESS(xsdk_present_begin(&target, 5000)))
			break;
		xsdk_gpu_set_target(w, h);
		const State s = snapshot();
		const auto now = std::chrono::steady_clock::now();
		xsdk_text_draw(kX, kY, 2, kAccent, s.title.c_str());
		// A spinner that moves every frame: if it stops, the title has frozen; if it moves, it's working.
		static const char *spin[] = { "|", "/", "-", "\\" };
		const std::string head = std::format("{}  {}   running {}", spin[(frame / 8) % 4], s.phase,
			elapsed_text(now - s.started));
		xsdk_text_draw(kX, kY + 60, 2, s.finished ? (s.failed ? kBad : kGood) : kWhite, head.c_str());
		int32_t y = kY + 110;
		if (s.total > 0) {
			const double frac = static_cast<double>(s.done) / static_cast<double>(s.total);
			xsdk_text_draw(kX, y, 2, kAccent, bar(frac > 1 ? 1 : frac, 40).c_str());
			const std::string count = std::format("{} / {} {}  ({:.0f}%)", s.done, s.total, s.unit, frac * 100);
			xsdk_text_draw(kX, y + 46, 1, kWhite, count.c_str());
			y += 70;
		} else if (s.done > 0) {
			xsdk_text_draw(kX, y, 1, kWhite, std::format("{} {}", s.done, s.unit).c_str());
			y += 24;
		}
		const std::string rate = rate_text(s);
		if (!rate.empty())
			xsdk_text_draw(kX, y, 1, kDim, rate.c_str());
		y += 40;
		for (const auto &n : s.notes) {
			xsdk_text_draw(kX, y, 1, kGood, n.c_str());
			y += 22;
		}
		// The title's last log lines, for debugging from the couch.
		const auto log = platform::recent_log();
		const int32_t log_top = static_cast<int32_t>(h) - kY - 8 * 22;
		xsdk_text_draw(kX, log_top - 26, 1, kDim, "Log:");
		const size_t first = log.size() > 8 ? log.size() - 8 : 0;
		for (size_t i = first; i < log.size(); i++) {
			std::string line = log[i].message;
			if (line.size() > 100)
				line = line.substr(0, 100) + "...";
			xsdk_text_draw(kX, log_top + static_cast<int32_t>(i - first) * 22, 1, kDim, line.c_str());
		}
		xsdk_gpu_resolve(target, w, h, kClear);
		xsdk_present_end();
		frame++;
	}
	xsdk_text_close();
	xsdk_present_close();
	xsdk_gpu_close();
}

} // namespace

bool start_screen()
{
	g_stop = false;
	g_ready = false;
	g_thread = std::thread(draw_loop);
	while (!g_ready)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return true;
}

void stop_screen()
{
	g_stop = true;
	if (g_thread.joinable())
		g_thread.join();
}

} // namespace xst::progress
