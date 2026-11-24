#include "core/config.hpp"

#include <fstream>
#include <random>
#include <sstream>

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
	if (in) {
		std::stringstream ss;
		ss << in.rdbuf();
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
	std::ofstream out(path_ + ".tmp", std::ios::trunc);
	out << cfg_.dump(1);
	out.close();
	std::rename((path_ + ".tmp").c_str(), path_.c_str());
	return true;
}

bool Config::save(std::string *error)
{
	std::string text;
	{
		std::lock_guard lock(mu_);
		text = cfg_.dump(1);
	}
	const std::string tmp = path_ + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << text;
		out.close();
		if (!out) {
			if (error)
				*error = "writing " + tmp + " failed";
			return false;
		}
	}
	std::remove(path_.c_str()); // the console can't rename over an existing file
	if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
		if (error)
			*error = "renaming config failed";
		return false;
	}
	return true;
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
