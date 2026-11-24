// The host build: the same daemon as the console's, on Linux, for development and the interop tests.
//   xsyncthing [--home DIR] [--name NAME] [--gui DIR] [--no-api]
// DIR defaults to $XSYNCTHING_HOME or ~/.local/state/xsyncthing. Stops on SIGINT/SIGTERM.
#include <csignal>
#include <cstring>
#include <format>
#include <thread>

#include <unistd.h>

#include "app.hpp"
#include "core/fs.hpp"
#include "core/version.hpp"
#include "platform/platform.hpp"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
} // namespace

int main(int argc, char **argv)
{
	xst::App::Options o;
	o.data_dir = xst::platform::data_dir();
	char host[256] = {};
	::gethostname(host, sizeof host - 1);
	o.device_name = host;
	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--home" && i + 1 < argc)
			o.data_dir = argv[++i];
		else if (a == "--name" && i + 1 < argc)
			o.device_name = argv[++i];
		else if (a == "--gui" && i + 1 < argc)
			o.gui_assets_dir = argv[++i];
		else if (a == "--no-api")
			o.enable_api = false;
		else if (a == "--version") {
			std::printf("%s %s\n", std::string(xst::client_name()).c_str(), std::string(xst::client_version()).c_str());
			return 0;
		} else {
			std::fprintf(stderr, "usage: %s [--home DIR] [--name NAME] [--gui DIR] [--no-api]\n", argv[0]);
			return 2;
		}
	}
	o.open_fs = [](const std::string &path) -> std::unique_ptr<xst::fs::Filesystem> {
		// XST_FS_RULES=fatx makes the host behave like the console's internal drive, for tests.
		xst::fs::VolumeRules rules;
		if (const char *r = std::getenv("XST_FS_RULES"); r && std::strcmp(r, "fatx") == 0) {
			rules.type = "fatx";
			rules.max_name_bytes = 42;
			rules.max_file_size = 0xFFFFFFFFLL;
			rules.forbidden_chars = R"(<>:"\|?*)";
			rules.keeps_permissions = false;
			rules.supports_symlinks = false;
			rules.mtime_window_ns = 2000000000LL;
		}
		return xst::fs::open_posix_with_rules(path, rules);
	};
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::signal(SIGPIPE, SIG_IGN);

	xst::App app(o);
	xst::register_services(app);
	std::string error;
	if (!app.start(&error)) {
		xst::platform::log("xsyncthing: " + error);
		return 1;
	}
	xst::platform::log(std::format("xsyncthing {} running; device ID {}", xst::client_version(),
		app.identity().id.to_string()));
	while (!g_stop)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	xst::platform::log("xsyncthing: stopping");
	app.stop();
	return 0;
}
