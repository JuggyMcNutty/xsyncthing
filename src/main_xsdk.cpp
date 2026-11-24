// The console title: the xsyncthing daemon (the same App as the host build) with the controller-driven console UI
// (src/ui) at 720p. Hold Back to quit; Xbox Home stops the daemon cleanly too. Logs checks the way xsdk's titles do,
// to xsyncthing.log beside the .xex. For unattended runs (tools/xbdm.py): autoexit.txt ends the title after 300
// frames, and stop.txt ends it at the next check.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "app.hpp"
#include "core/fs.hpp"
#include "core/version.hpp"
#include "imgui.h"
#include "imgui_impl_xsdk.h"
#include "platform/platform.hpp"
#include "platform/xsdk/console.hpp"
#include "ui/console_ui.hpp"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

namespace {

constexpr uint32_t kSplashMs = 1500;
constexpr uint32_t kBuffers = 2;
constexpr uint32_t kSwapInterval = 1;
// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md, "The Guide, milestone 37").
constexpr uint32_t kBeginTimeoutMs = 5000;
constexpr uint32_t kClearArgb = 0xFF0B1620;
constexpr uint32_t kAutoExitFrames = 300;

uint32_t passed;
uint32_t failed;
std::atomic<bool> g_home{ false };

void check(const char *name, bool ok)
{
	xsdk_log_write("%s: %s", ok ? "PASS" : "FAIL", name);
	passed += ok;
	failed += !ok;
}

bool exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

// Writes the frame just presented to game:/shot-<n>.ppm (for checking the UI as the console draws it). The frame is in
// memory as a tiled texture, so it's read back untiled, row by row, as xsdk's template checks its first frame.
void save_screenshot(uint32_t address, uint32_t width, uint32_t height, int n)
{
	XsdkTexture frame;
	if (!NT_SUCCESS(xsdk_texture_init(&frame, XSDK_FORMAT_A8R8G8B8, width, height, 1, true)))
		return;
	xsdk_texture_attach(&frame, xsdk_present_pixels(address));
	const std::string path = std::format("game:/shot-{}.ppm", n);
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height);
	std::vector<uint32_t> row(width);
	std::vector<uint8_t> rgb(width * 3);
	for (uint32_t y = 0; y < height; y++) {
		xsdk_texture_download_rows(&frame, 0, y, 1, row.data(), width * 4);
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t p = __builtin_bswap32(row[x]); // stored B, G, R, A
			rgb[x * 3] = static_cast<uint8_t>(p >> 16);
			rgb[x * 3 + 1] = static_cast<uint8_t>(p >> 8);
			rgb[x * 3 + 2] = static_cast<uint8_t>(p);
		}
		std::fwrite(rgb.data(), 1, rgb.size(), f);
	}
	std::fclose(f);
	xsdk_log_write("xsyncthing: saved %s", path.c_str());
}

// Xbox Home: the title is frozen; only flag it. The database journal keeps it consistent whatever happens next.
void on_home() { g_home = true; }

// Draws the UI each frame until the user quits, Xbox Home, stop.txt, or autoexit.
void run(xst::App &app, uint32_t width, uint32_t height)
{
	xst::ui::ConsoleUi ui(app);
	const bool auto_exit = exists("game:/autoexit.txt");
	// shot.txt: step through every screen with RB and save each one (a check of the UI on real hardware).
	const bool shots = exists("game:/shot.txt");
	int shot = 0;
	for (uint32_t frame = 0;; frame++) {
		if (g_home)
			return;
		if (auto_exit && frame >= (shots ? 60u + 90u * 8u : kAutoExitFrames)) {
			check("xsyncthing: presented frames until autoexit", true);
			return;
		}
		if (frame % 60 == 0 && exists("game:/stop.txt")) {
			xsdk_log_write("xsyncthing: stop.txt found, stopping");
			::remove("game:/stop.txt");
			return;
		}
		if (xst::platform::restart_requested() == xst::platform::RestartKind::shutdown) {
			xsdk_log_write("xsyncthing: shutdown requested from the Web GUI");
			return;
		}
		if (shots && frame > 60 && frame % 45 == 0 && shot < 7) {
			// Simulate RB: the backend reads controller 1, so pass a state with the shoulder held for one frame.
			INPUT_GAMEPAD pad = {};
			if (frame % 90 == 0)
				pad.buttons = INPUT_GAMEPAD_RIGHT_SHOULDER;
			ImGui_ImplXsdk_ProcessGamepad(&pad);
		}
		ImGui_ImplXsdk_NewFrame(width, height);
		ImGui::NewFrame();
		const bool keep = ui.frame(static_cast<float>(width), static_cast<float>(height));
		ImGui::Render();
		uint32_t target = 0;
		const NTSTATUS status = xsdk_present_begin(&target, kBeginTimeoutMs);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("xsyncthing: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		xsdk_gpu_set_target(width, height);
		ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
		xsdk_gpu_resolve(target, width, height, kClearArgb);
		if (shots && frame > 60 && frame % 90 == 45 && shot < 7) {
			xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
			save_screenshot(target, width, height, shot++);
		}
		xsdk_present_end();
		if (frame == 0)
			check("xsyncthing: the first UI frame was presented", true);
		if (!keep) {
			xsdk_log_write("xsyncthing: quit from the UI");
			return;
		}
	}
}

} // namespace

int main()
{
	xst::platform::log(std::format("xsyncthing: {} {} starting", xst::client_name(), xst::client_version()));
	const NTSTATUS splash = xsdk_splash_show(kSplashMs);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("xsyncthing: xsdk_splash_show failed: 0x%08X", (unsigned)splash);

	NTSTATUS status = xsdk_gpu_open();
	check("xsyncthing: xsdk_gpu_open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return 1;
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, kBuffers, kSwapInterval, &width, &height);
	check("xsyncthing: xsdk_present_open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return 1;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	xst::ui::ConsoleUi::setup_style();
	const bool imgui = ImGui_ImplXsdk_Init();
	check("xsyncthing: ImGui_ImplXsdk_Init", imgui);

	xst::console::on_home(on_home);
	xst::App::Options o;
	o.data_dir = xst::platform::data_dir();
	o.device_name = "Xbox 360";
	o.gui_assets_dir = "game:/gui";
	o.open_fs = xst::fs::open_console;
	xst::App app(o);
	xst::register_services(app);
	std::string error;
	const bool started = app.start(&error);
	check("xsyncthing: the daemon starts", started);
	if (!started)
		xsdk_log_write("xsyncthing: %s", error.c_str());

	if (imgui && started)
		run(app, width, height);

	// A watchdog: if stopping the daemon hangs, end the title anyway rather than leave the console stuck in it.
	std::atomic<bool> stopped{ false };
	std::thread watchdog([&] {
		for (int i = 0; i < 150 && !stopped; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!stopped) {
			xsdk_log_write("xsyncthing: stopping took over 15 s; ending the title");
			xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed + 1);
			_exit(2);
		}
	});
	const auto stop_start = std::chrono::steady_clock::now();
	app.stop();
	stopped = true;
	watchdog.join();
	xsdk_log_write("xsyncthing: the daemon stopped in %lld ms",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_start)
			.count());
	check("xsyncthing: the daemon stops", true);

	if (imgui)
		ImGui_ImplXsdk_Shutdown();
	ImGui::DestroyContext();
	xsdk_present_close();
	xsdk_gpu_close();
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed);
	return failed ? 1 : 0;
}
