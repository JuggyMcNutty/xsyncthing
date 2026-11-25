#include "ui/view.hpp"

#include <algorithm>
#include <cstdio>

#include "api/api.hpp"
#include "core/bcrypt.hpp"
#include "core/version.hpp"
#include "net/addresses.hpp"
#include "net/global_discovery.hpp"
#include "net/local_discovery.hpp"
#include "net/relay.hpp"

namespace xst::ui {

namespace {

std::string device_name(App &app, const DeviceId &id)
{
	const json d = app.config().device(id);
	if (!d.is_null() && !d.value("name", "").empty())
		return d.value("name", "");
	return id.short_string();
}

} // namespace

std::string human_bytes(int64_t bytes)
{
	const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = static_cast<double>(bytes);
	int u = 0;
	while (v >= 1024 && u < 4) {
		v /= 1024;
		u++;
	}
	char buf[32];
	std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
	return buf;
}

Snapshot snapshot(App &app)
{
	Snapshot s;
	Config &cfg = app.config();
	Model &model = app.model();
	s.device_id = cfg.my_id().to_string();
	s.version = std::string(client_version());
	const auto ips = lan_ipv4_addresses();
	s.address = ips.empty() ? "" : ips.front();
	const json gui = cfg.gui();
	s.api_key = gui.value("apiKey", "");
	s.gui_user = gui.value("user", "");
	s.gui_has_password = !gui.value("password", "").empty();
	if (auto *api = app.api())
		s.gui_url = api->url();
	const json opts = cfg.options();
	s.local_discovery = opts.value("localAnnounceEnabled", true);
	s.global_discovery = opts.value("globalAnnounceEnabled", true);
	s.relays = opts.value("relaysEnabled", true);
	s.listen_address = std::to_string(app.connections().listen_port());
	if (auto *r = app.relays())
		s.relay = r->joined_uri();
	if (auto *g = app.global_discovery())
		for (const auto &[server, e] : g->errors())
			if (!e.empty())
				s.discovery_errors.push_back(server + ": " + e);
	if (auto *l = app.local_discovery(); l && !l->error().empty())
		s.discovery_errors.push_back("local: " + l->error());

	for (const auto &d : cfg.devices()) {
		const auto id = DeviceId::parse(d.value("deviceID", ""));
		if (!id)
			continue;
		if (*id == cfg.my_id()) {
			s.device_name = d.value("name", "");
			continue;
		}
		DeviceRow row;
		row.id = id->to_string();
		row.short_id = id->short_string();
		row.name = d.value("name", "").empty() ? row.short_id : d.value("name", "");
		row.paused = d.value("paused", false);
		for (const auto &c : model.connections())
			if (c->device() == *id) {
				row.connected = true;
				row.address = c->address();
				row.type = c->type();
				row.client = c->client_version();
			}
		row.completion = model.completion("", *id).value("completion", 100.0);
		s.devices.push_back(row);
	}

	for (const auto &f : cfg.folders()) {
		FolderRow row;
		row.id = f.value("id", "");
		row.label = f.value("label", "").empty() ? row.id : f.value("label", "");
		row.path = f.value("path", "");
		row.type = f.value("type", "sendreceive");
		row.paused = f.value("paused", false);
		const auto st = model.folder_state(row.id);
		row.state = row.paused ? "paused" : st.state;
		row.error = st.error;
		row.errors = static_cast<int>(st.pull_errors.size());
		row.scan_files = st.scan_files;
		row.pull_done = st.pull_done;
		row.pull_total = st.pull_total;
		const json status = model.folder_status(row.id);
		row.global_bytes = status.value("globalBytes", 0);
		row.need_bytes = status.value("needBytes", 0);
		row.need_items = status.value("needTotalItems", 0);
		row.local_files = status.value("localFiles", 0);
		row.completion = model.completion(row.id, cfg.my_id()).value("completion", 100.0);
		for (const auto &fd : f["devices"]) {
			const auto did = DeviceId::parse(fd.value("deviceID", ""));
			if (did && *did != cfg.my_id())
				row.shared_with.push_back(device_name(app, *did));
		}
		s.folders.push_back(row);
	}

	const json pd = model.pending_devices();
	for (auto it = pd.begin(); it != pd.end(); ++it)
		s.pending_devices.push_back({ it.key(), it.value().value("name", ""), it.value().value("address", "") });
	const json pf = model.pending_folders(DeviceId());
	for (auto it = pf.begin(); it != pf.end(); ++it)
		for (auto d = it.value()["offeredBy"].begin(); d != it.value()["offeredBy"].end(); ++d) {
			const auto did = DeviceId::parse(d.key());
			s.pending_folders.push_back({ it.key(), d.value().value("label", it.key()), d.key(),
				did ? device_name(app, *did) : d.key() });
		}
	const json errs = model.system_errors();
	if (errs.is_array())
		for (const auto &e : errs)
			s.system_errors.push_back(e.value("message", ""));
	return s;
}

bool add_device(App &app, const std::string &id_text, const std::string &name, std::string *error)
{
	const auto id = DeviceId::parse(id_text);
	if (!id || id->empty()) {
		if (error)
			*error = "That isn't a valid device ID.";
		return false;
	}
	if (*id == app.config().my_id()) {
		if (error)
			*error = "That's this console's own device ID.";
		return false;
	}
	app.config().modify([&](json &c) {
		json d = c["defaults"]["device"];
		d["deviceID"] = id->to_string();
		d["name"] = name;
		bool found = false;
		for (auto &x : c["devices"])
			if (DeviceId::parse(x.value("deviceID", "")) == *id) {
				x["name"] = name.empty() ? x.value("name", "") : name;
				found = true;
			}
		if (!found)
			c["devices"].push_back(d);
	});
	app.model().dismiss_pending_device(*id);
	app.connections().dial_now();
	return true;
}

bool accept_pending_device(App &app, const std::string &id, const std::string &name)
{
	return add_device(app, id, name, nullptr);
}

void dismiss_pending_device(App &app, const std::string &id)
{
	if (auto d = DeviceId::parse(id))
		app.model().dismiss_pending_device(*d);
}

std::string default_folder_path(App &app, const std::string &label)
{
	std::string base = app.config().defaults()["folder"].value("path", "hdd:/xsyncthing/folders");
	std::string name;
	for (char c : label)
		name.push_back(std::string("<>:\"\\|?*/").find(c) == std::string::npos && static_cast<unsigned char>(c) >= 0x20 ? c : '_');
	while (!name.empty() && (name.back() == '.' || name.back() == ' '))
		name.pop_back();
	if (name.empty())
		name = "folder";
	if (name.size() > 42)
		name.resize(42);
	return base + "/" + name;
}

bool share_folder(App &app, const std::string &folder_id, const std::string &label, const std::string &path,
	const std::string &device_id, std::string *error)
{
	const auto dev = DeviceId::parse(device_id);
	if (!device_id.empty() && !dev) {
		if (error)
			*error = "Unknown device.";
		return false;
	}
	if (folder_id.empty()) {
		if (error)
			*error = "The folder needs an ID.";
		return false;
	}
	const DeviceId me = app.config().my_id();
	app.config().modify([&](json &c) {
		json *f = nullptr;
		for (auto &x : c["folders"])
			if (x.value("id", "") == folder_id)
				f = &x;
		if (!f) {
			json n = c["defaults"]["folder"];
			n["id"] = folder_id;
			n["label"] = label.empty() ? folder_id : label;
			n["path"] = path;
			n["devices"] = json::array({ { { "deviceID", me.to_string() }, { "introducedBy", "" }, { "encryptionPassword", "" } } });
			c["folders"].push_back(n);
			f = &c["folders"].back();
		}
		if (dev) {
			bool has = false;
			for (const auto &d : (*f)["devices"])
				if (DeviceId::parse(d.value("deviceID", "")) == *dev)
					has = true;
			if (!has)
				(*f)["devices"].push_back({ { "deviceID", dev->to_string() }, { "introducedBy", "" }, { "encryptionPassword", "" } });
		}
	});
	return true;
}

bool accept_pending_folder(App &app, const PendingFolder &p, const std::string &path, std::string *error)
{
	if (!share_folder(app, p.folder_id, p.label, path, p.device_id, error))
		return false;
	if (auto d = DeviceId::parse(p.device_id))
		app.model().dismiss_pending_folder(p.folder_id, *d);
	return true;
}

void dismiss_pending_folder(App &app, const PendingFolder &p)
{
	const auto d = DeviceId::parse(p.device_id);
	// Upstream "Ignore" adds it to the device's ignoredFolders so it isn't offered again.
	if (d)
		app.config().modify([&](json &c) {
			for (auto &x : c["devices"])
				if (DeviceId::parse(x.value("deviceID", "")) == *d)
					x["ignoredFolders"].push_back({ { "time", rfc3339_now() }, { "id", p.folder_id }, { "label", p.label } });
		});
	app.model().dismiss_pending_folder(p.folder_id, d.value_or(DeviceId()));
}

void set_folder_paused(App &app, const std::string &folder_id, bool paused)
{
	app.config().modify([&](json &c) {
		for (auto &f : c["folders"])
			if (f.value("id", "") == folder_id)
				f["paused"] = paused;
	});
}

void set_device_paused(App &app, const std::string &device_id, bool paused)
{
	const auto d = DeviceId::parse(device_id);
	app.config().modify([&](json &c) {
		for (auto &x : c["devices"])
			if (DeviceId::parse(x.value("deviceID", "")) == d)
				x["paused"] = paused;
	});
}

void rescan(App &app, const std::string &folder_id) { app.model().scan(folder_id); }

void remove_folder(App &app, const std::string &folder_id)
{
	app.config().modify([&](json &c) {
		auto &l = c["folders"];
		l.erase(std::remove_if(l.begin(), l.end(), [&](const json &f) { return f.value("id", "") == folder_id; }), l.end());
	});
	app.model().db().drop_folder(folder_id);
}

void remove_device(App &app, const std::string &device_id)
{
	const auto d = DeviceId::parse(device_id);
	app.config().modify([&](json &c) {
		auto &l = c["devices"];
		l.erase(std::remove_if(l.begin(), l.end(), [&](const json &x) { return DeviceId::parse(x.value("deviceID", "")) == d; }),
			l.end());
		for (auto &f : c["folders"]) {
			auto &fd = f["devices"];
			fd.erase(std::remove_if(fd.begin(), fd.end(), [&](const json &x) { return DeviceId::parse(x.value("deviceID", "")) == d; }),
				fd.end());
		}
	});
}

void set_option(App &app, const std::string &key, const json &value)
{
	app.config().modify([&](json &c) { c["options"][key] = value; });
}

void set_gui_enabled(App &app, bool enabled)
{
	app.config().modify([&](json &c) { c["gui"]["enabled"] = enabled; });
}

void set_gui_password(App &app, const std::string &user, const std::string &password)
{
	app.config().modify([&](json &c) {
		c["gui"]["user"] = user;
		c["gui"]["password"] = password.empty() ? "" : bcrypt::generate(password, 10);
	});
}

} // namespace xst::ui
