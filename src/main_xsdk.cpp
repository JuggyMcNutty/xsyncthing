// The console title: runs the xsyncthing daemon (the same App as the host build) and, until the console UI lands,
// a status screen at 720p: version, address, device ID, folders and connections. Back exits; so does Xbox Home,
// which stops the daemon cleanly first. Logs checks the way xsdk's titles do, to xsyncthing.log beside the .xex.
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <format>
#include <string>

#include <sys/stat.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "app.hpp"
#include "core/version.hpp"
#include "platform/platform.hpp"
#include "platform/xsdk/console.hpp"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/net.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/text.h"

namespace {

constexpr uint32_t kSplashMs = 1500;
constexpr uint32_t kBuffers = 2;
constexpr uint32_t kSwapInterval = 1;
// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md, "The Guide, milestone 37").
constexpr uint32_t kBeginTimeoutMs = 5000;
constexpr uint32_t kClearArgb = 0xFF0B2A3C;
constexpr uint32_t kTextArgb = 0xFFFFFFFF;
constexpr uint32_t kDimArgb = 0xFF9FB4C0;
// The title-safe area: 10% in from each edge of a 1280x720 frame.
constexpr int32_t kSafeX = 128;
constexpr int32_t kSafeY = 72;

uint32_t passed;
uint32_t failed;

void check(const char *name, bool ok)
{
	xsdk_log_write("%s: %s", ok ? "PASS" : "FAIL", name);
	passed += ok;
	failed += !ok;
}

std::string address_text() { return xst::console::address(); }

xst::App *g_app = nullptr;
std::atomic<bool> g_home{ false };

// Xbox Home: the title is frozen; only flag it. The database is in TRUNCATE journal mode, so a stop that never runs
// still leaves a consistent database (SQLite replays or discards the journal on the next open).
void on_home() { g_home = true; }

std::string status_text(xst::App &app)
{
	std::string s;
	for (const auto &f : app.config().folders()) {
		const std::string id = f.value("id", "");
		const auto st = app.model().folder_state(id);
		s += std::format("Folder {:<20} {:<10} {}\n", f.value("label", id), st.state, f.value("path", ""));
	}
	for (const auto &c : app.model().connections())
		s += std::format("Device {} connected ({}, {})\n", c->device().short_string(), c->address(), c->type());
	const auto pending = app.model().pending_devices();
	for (auto it = pending.begin(); it != pending.end(); ++it)
		s += std::format("Pending device {} \"{}\" - add it from the Web GUI\n", it.key().substr(0, 7),
			it.value().value("name", ""));
	return s.empty() ? "No folders yet: open the Web GUI to add devices and folders.\n" : s;
}

// For automated runs (tools/xbdm.py): with game:\autoexit.txt present, the title ends by itself after this many
// frames, so its log is closed and can be fetched.
constexpr uint32_t kAutoExitFrames = 300;

bool auto_exit_requested()
{
	struct stat st;
	return stat("game:/autoexit.txt", &st) == 0;
}

void run(uint32_t width, uint32_t height)
{
	std::string address = address_text();
	std::string status_lines;
	const bool auto_exit = auto_exit_requested();
	for (uint32_t frame = 0;; frame++) {
		if (auto_exit && frame >= kAutoExitFrames) {
			xsdk_log_write("xsyncthing: address %s", address.c_str());
			check("xsyncthing: presented frames until autoexit", true);
			return;
		}
		INPUT_STATE input = {};
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return;
		// The address can take a few seconds to come from DHCP: look again about once a second.
		if (frame % 60 == 0)
			address = address_text();

		uint32_t target;
		const NTSTATUS status = xsdk_present_begin(&target, kBeginTimeoutMs);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("xsyncthing: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		if (g_home)
			return;
		xsdk_gpu_set_target(width, height);
		const std::string title = std::format("{} {}", xst::client_name(), xst::client_version());
		xsdk_text_draw(kSafeX, kSafeY, 3, kTextArgb, title.c_str());
		xsdk_text_draw(kSafeX, kSafeY + 70, 1, kDimArgb, "Syncthing for the Xbox 360 - pre-alpha");
		std::string info = std::format("Address:   {}\nData:      {}", address, xst::platform::data_dir());
		if (g_app)
			info += std::format("\nDevice ID: {}\nWeb GUI:   http://{}:8384", g_app->identity().id.to_string(),
				address);
		xsdk_text_draw(kSafeX, kSafeY + 100, 1, kTextArgb, info.c_str());
		if (g_app && frame % 30 == 0)
			status_lines = status_text(*g_app);
		xsdk_text_draw(kSafeX, kSafeY + 200, 1, kTextArgb, status_lines.c_str());
		xsdk_text_draw(kSafeX, (int32_t)height - kSafeY - 20, 1, kDimArgb, "Back: exit");
		xsdk_gpu_resolve(target, width, height, kClearArgb);
		xsdk_present_end();
		if (frame == 0)
			check("xsyncthing: the first frame was presented", true);
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
	// Xenia has no display controller, so this fails there (xsdk:docs/GETTING_STARTED.md, "Xenia").
	check("xsyncthing: xsdk_present_open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("xsyncthing: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	status = xsdk_text_open();
	check("xsyncthing: xsdk_text_open", NT_SUCCESS(status));

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
	if (started)
		g_app = &app;
	else
		xsdk_log_write("xsyncthing: %s", error.c_str());

	if (NT_SUCCESS(status)) {
		run(width, height);
		xsdk_text_close();
	}
	g_app = nullptr;
	app.stop();
	check("xsyncthing: the daemon stops", true);
	xsdk_present_close();
	xsdk_gpu_close();
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed);
	return failed ? 1 : 0;
}
