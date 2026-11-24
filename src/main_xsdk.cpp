// The console title. For now (Phase 0) it is the hello-world bring-up: it shows the version, the console's IPv4
// address and the data folder at 720p until Back is pressed, and logs checks the way xsdk's titles do, to
// xsyncthing.log beside the .xex.
#include <cinttypes>
#include <cstdio>
#include <format>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "core/version.hpp"
#include "platform/platform.hpp"
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

std::string address_text()
{
	in_addr address{};
	const int error = xsdk_net_address(&address);
	if (error == EAGAIN)
		return "getting an address...";
	if (error != 0)
		return std::format("no network (error {})", error);
	char text[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &address, text, sizeof text);
	return text;
}

void run(uint32_t width, uint32_t height)
{
	std::string address = address_text();
	for (uint32_t frame = 0;; frame++) {
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
		xsdk_gpu_set_target(width, height);
		const std::string title = std::format("{} {}", xst::client_name(), xst::client_version());
		xsdk_text_draw(kSafeX, kSafeY, 3, kTextArgb, title.c_str());
		xsdk_text_draw(kSafeX, kSafeY + 90, 1, kDimArgb, "Syncthing for the Xbox 360 - pre-alpha");
		const std::string info = std::format("Address:   {}\nData:      {}", address, xst::platform::data_dir());
		xsdk_text_draw(kSafeX, kSafeY + 140, 2, kTextArgb, info.c_str());
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
	if (NT_SUCCESS(status)) {
		run(width, height);
		xsdk_text_close();
	}
	xsdk_present_close();
	xsdk_gpu_close();
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed);
	return failed ? 1 : 0;
}
