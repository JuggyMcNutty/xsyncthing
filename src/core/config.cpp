#include "core/config.hpp"

#include <fstream>
#include <random>
#include <sstream>
#include <utility>

namespace xst {

namespace {

const char *kDefaults =
#include "core/defaults/config_json.inc"
	;

std::string random_string(size_t n)
{
	static const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	std::random_device rd;
	std::string s;
	for (size_t i = 0; i < n; i++)
		s.push_back(chars[rd() % 62]);
	return s;
}

void merge_defaults(json &target, const json &defaults)
{
	if (!target.is_object() || !defaults.is_object())
		return;
	for (auto it = defaults.begin(); it != defaults.end(); ++it) {
		if (!target.contains(it.key()))
			target[it.key()] = it.value();
		else if (it.value().is_object() && target[it.key()].is_object())
			merge_defaults(target[it.key()], it.value());
	}
}

} // namespace

json Config::default_config() { return json::parse(kDefaults); }

std::vector<std::string> Config::enforce_supported(json &cfg)
{
	std::vector<std::string> notes;
	for (auto &f : cfg["folders"]) {
		const std::string id = f.value("id", "");
		if (f.contains("versioning") && !f["versioning"].value("type", "").empty()) {
			notes.push_back("folder " + id + ": file versioning is not supported on the console; turned off");
			f["versioning"]["type"] = "";
		}
		if (f.value("type", "") == "receiveencrypted") {
			notes.push_back("folder " + id + ": receive-encrypted folders are not supported; paused");
			f["paused"] = true;
		}
		for (auto &d : f["devices"])
			if (!d.value("encryptionPassword", "").empty()) {
				notes.push_back("folder " + id + ": sharing with an untrusted (encrypted) device is not supported");
				d["encryptionPassword"] = "";
			}
	}
	for (auto &d : cfg["devices"])
		if (d.value("untrusted", false)) {
			notes.push_back("device " + d.value("name", d.value("deviceID", "")) + ": untrusted devices are not supported; paused");
			d["untrusted"] = false;
			d["paused"] = true;
		}
	return notes;
}
json Config::default_folder() { return default_config()["defaults"]["folder"]; }
json Config::default_device() { return default_config()["defaults"]["device"]; }

void Config::fill_folder(json &f)
{
	merge_defaults(f, default_folder());
	if (f["label"].get<std::string>().empty())
		f["label"] = f["id"];
	for (auto &d : f["devices"]) {
		if (!d.contains("introducedBy"))
			d["introducedBy"] = "";
		if (!d.contains("encryptionPassword"))
			d["encryptionPassword"] = "";
	}
}

void Config::fill_device(json &d) { merge_defaults(d, default_device()); }

bool Config::load(const std::string &path, const DeviceId &my_id, const std::string &my_name, std::string *error)
{
	std::lock_guard lock(mu_);
	path_ = path;
	my_id_ = my_id;
	std::ifstream in(path);
	if (!in) {
		// An interrupted save leaves only the new file, still named .tmp.
		in.open(path + ".tmp");
	}
	if (in) {
		std::stringstream ss;
		ss << in.rdbuf();
		// Close before writing below: the console refuses to remove or replace a file that's still open.
		in.close();
		try {
			cfg_ = json::parse(ss.str());
		} catch (const std::exception &e) {
			if (error)
				*error = std::string("config.json: ") + e.what();
			return false;
		}
	} else {
		cfg_ = default_config();
	}
	merge_defaults(cfg_, default_config());
	if (cfg_["gui"]["apiKey"].get<std::string>().empty())
		cfg_["gui"]["apiKey"] = random_string(32);
	// Our own device is always in the list, as upstream keeps it.
	bool have_self = false;
	for (auto &d : cfg_["devices"])
		if (DeviceId::parse(d.value("deviceID", "")) == my_id)
			have_self = true;
	if (!have_self) {
		json self = default_device();
		self["deviceID"] = my_id.to_string();
		self["name"] = my_name;
		cfg_["devices"].push_back(self);
	}
	for (auto &f : cfg_["folders"])
		fill_folder(f);
	for (auto &d : cfg_["devices"])
		fill_device(d);
	enforce_supported(cfg_);
	return write_locked(error);
}

bool Config::write_locked(std::string *error)
{
	// Write beside, then swap in. The console can't rename over an existing file, so the old one goes first; if
	// power is lost between the two, load() finds config.json.tmp and takes it.
	const std::string tmp = path_ + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << cfg_.dump(1);
		out.close();
		if (!out) {
			if (error)
				*error = "writing " + tmp + " failed";
			return false;
		}
	}
	std::remove(path_.c_str());
	if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
		if (error)
			*error = "renaming " + tmp + " failed";
		return false;
	}
	return true;
}

bool Config::save(std::string *error)
{
	std::lock_guard lock(mu_);
	return write_locked(error);
}

json Config::get() const
{
	std::lock_guard lock(mu_);
	return cfg_;
}

void Config::replace(json cfg)
{
	{
		std::lock_guard lock(mu_);
		merge_defaults(cfg, default_config());
		for (auto &f : cfg["folders"])
			fill_folder(f);
		for (auto &d : cfg["devices"])
			fill_device(d);
		last_notes_ = enforce_supported(cfg);
		cfg_ = std::move(cfg);
	}
	save();
	notify();
}

void Config::modify(const std::function<void(json &)> &fn)
{
	{
		std::lock_guard lock(mu_);
		fn(cfg_);
		for (auto &f : cfg_["folders"])
			fill_folder(f);
		for (auto &d : cfg_["devices"])
			fill_device(d);
		last_notes_ = enforce_supported(cfg_);
	}
	save();
	notify();
}

json Config::folder(const std::string &id) const
{
	std::lock_guard lock(mu_);
	for (const auto &f : cfg_["folders"])
		if (f.value("id", "") == id)
			return f;
	return nullptr;
}

json Config::device(const DeviceId &id) const
{
	std::lock_guard lock(mu_);
	for (const auto &d : cfg_["devices"])
		if (DeviceId::parse(d.value("deviceID", "")) == id)
			return d;
	return nullptr;
}

std::vector<json> Config::folders() const
{
	std::lock_guard lock(mu_);
	return std::vector<json>(cfg_["folders"].begin(), cfg_["folders"].end());
}

std::vector<json> Config::devices() const
{
	std::lock_guard lock(mu_);
	return std::vector<json>(cfg_["devices"].begin(), cfg_["devices"].end());
}

json Config::options() const
{
	std::lock_guard lock(mu_);
	return cfg_["options"];
}

json Config::gui() const
{
	std::lock_guard lock(mu_);
	return cfg_["gui"];
}

json Config::defaults() const
{
	std::lock_guard lock(mu_);
	return cfg_["defaults"];
}

bool Config::folder_shared_with(const std::string &folder, const DeviceId &device) const
{
	const json f = this->folder(folder);
	if (f.is_null())
		return false;
	for (const auto &d : f["devices"])
		if (DeviceId::parse(d.value("deviceID", "")) == device)
			return true;
	return false;
}

std::vector<std::string> Config::take_notes()
{
	std::lock_guard lock(mu_);
	return std::exchange(last_notes_, {});
}

void Config::on_change(std::function<void()> fn)
{
	std::lock_guard lock(mu_);
	listeners_.push_back(std::move(fn));
}

void Config::notify()
{
	std::vector<std::function<void()>> ls;
	{
		std::lock_guard lock(mu_);
		ls = listeners_;
	}
	for (auto &l : ls)
		l();
}

} // namespace xst
