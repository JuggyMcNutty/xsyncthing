// Step 24: the console UI and the REST API share one source of truth. Each change made through one shows in the other
// at once, because both read and write the same Config and Model.
#include <filesystem>

#include <unistd.h>

#include "api/api.hpp"
#include "app.hpp"
#include "core/fs.hpp"
#include "test.hpp"
#include "ui/view.hpp"

using namespace xst;
namespace sfs = std::filesystem;

namespace {

struct TestApp {
	sfs::path dir;
	std::unique_ptr<App> app;
	std::unique_ptr<ApiServer> api;
	TestApp()
	{
		dir = sfs::temp_directory_path() / ("xst-ui-" + std::to_string(::getpid()));
		sfs::remove_all(dir);
		sfs::create_directories(dir);
		App::Options o;
		o.data_dir = dir.string();
		o.device_name = "ui-test";
		o.open_fs = [](const std::string &p) { return fs::open_posix(p); };
		o.enable_api = false;
		app = std::make_unique<App>(o);
		// Keep the network quiet: no listener collisions, no discovery.
		std::string error;
		CHECK(app->start(&error));
		app->config().modify([](json &c) {
			c["options"]["globalAnnounceEnabled"] = false;
			c["options"]["localAnnounceEnabled"] = false;
			c["options"]["relaysEnabled"] = false;
		});
		api = std::make_unique<ApiServer>(*app); // not listening: requests go straight to handle()
	}
	~TestApp()
	{
		app->stop();
		sfs::remove_all(dir);
	}
	json rest(const std::string &method, const std::string &path, const json &body = nullptr)
	{
		http::Request r;
		r.method = method;
		const auto q = path.find('?');
		r.path = path.substr(0, q);
		if (q != std::string::npos) {
			std::string qs = path.substr(q + 1);
			size_t pos = 0;
			while (pos <= qs.size()) {
				auto amp = qs.find('&', pos);
				if (amp == std::string::npos)
					amp = qs.size();
				const std::string kv = qs.substr(pos, amp - pos);
				const auto eq = kv.find('=');
				if (!kv.empty())
					r.params[kv.substr(0, eq)] = eq == std::string::npos ? "" : kv.substr(eq + 1);
				pos = amp + 1;
			}
		}
		r.headers["x-api-key"] = app->config().gui().value("apiKey", "");
		if (!body.is_null())
			r.body = body.dump();
		const auto resp = api->handle(r);
		return resp.body.empty() || (resp.body[0] != '{' && resp.body[0] != '[') ? json(resp.status) : json::parse(resp.body);
	}
};

const char *kPeer = "FTPHMOV-CBA6IRW-7B4E5IG-ALLMAXE-VOKUCGP-PT2POKE-GTWP4GE-SLMXGAI";

} // namespace

TEST(ui_and_api_share_one_model)
{
	TestApp t;
	// The UI adds a device: the REST API sees it.
	std::string error;
	CHECK(ui::add_device(*t.app, kPeer, "my pc", &error));
	const json devs = t.rest("GET", "/rest/config/devices");
	CHECK(std::any_of(devs.begin(), devs.end(), [](const json &d) { return d["deviceID"] == kPeer && d["name"] == "my pc"; }));

	// The REST API renames it: the UI's next snapshot shows the new name.
	t.rest("PATCH", std::string("/rest/config/devices/") + kPeer, json{ { "name", "renamed in web gui" } });
	auto s = ui::snapshot(*t.app);
	CHECK(std::any_of(s.devices.begin(), s.devices.end(), [](const ui::DeviceRow &d) { return d.name == "renamed in web gui"; }));

	// The UI shares a folder with it: the API's config and db/status have it.
	const std::string path = (t.dir / "share").string();
	CHECK(ui::share_folder(*t.app, "ui-folder", "From the UI", path, kPeer, &error));
	const json f = t.rest("GET", "/rest/config/folders/ui-folder");
	CHECK(f["label"] == "From the UI");
	CHECK(f["devices"].size() == 2u);
	const json st = t.rest("GET", "/rest/db/status?folder=ui-folder");
	CHECK(st.contains("state"));

	// Paused from the API: the UI shows it paused.
	t.rest("PATCH", "/rest/config/folders/ui-folder", json{ { "paused", true } });
	s = ui::snapshot(*t.app);
	const auto it = std::find_if(s.folders.begin(), s.folders.end(), [](const ui::FolderRow &r) { return r.id == "ui-folder"; });
	CHECK(it != s.folders.end() && it->paused && it->state == "paused");

	// The GUI password set from the UI is what the API checks (bcrypt in config.gui).
	ui::set_gui_password(*t.app, "admin", "pw");
	CHECK(t.app->config().gui().value("password", "").starts_with("$2a$10$"));
	CHECK(ui::snapshot(*t.app).gui_has_password);

	// Removed in the UI: gone from the API.
	ui::remove_device(*t.app, kPeer);
	const json devs2 = t.rest("GET", "/rest/config/devices");
	CHECK(std::none_of(devs2.begin(), devs2.end(), [](const json &d) { return d["deviceID"] == kPeer; }));
}

TEST(ui_default_folder_path_fits_fatx)
{
	TestApp t;
	const auto p = ui::default_folder_path(*t.app, "A label: with *bad* chars and far, far too long for a FATX name.");
	const auto name = p.substr(p.rfind('/') + 1);
	CHECK(name.size() <= 42);
	CHECK(name.find_first_of("<>:\"\\|?*") == std::string::npos);
}

TEST(ui_human_bytes)
{
	CHECK_EQ(ui::human_bytes(830), "830 B");
	CHECK_EQ(ui::human_bytes(1536), "1.5 KiB");
	CHECK_EQ(ui::human_bytes(3LL << 30), "3.0 GiB");
}
