#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/device_id.hpp"

namespace xst {

using json = nlohmann::json;

// The configuration, kept as JSON in exactly the shape of Syncthing's /rest/config (config version 52), so the stock
// Web GUI reads and writes it unchanged. Saved as config.json in the data directory (upstream keeps the same
// structure as config.xml). Thread-safe; listeners run after each change.
class Config {
public:
	// Loads path, or starts from the defaults (src/core/defaults/config.json) if it doesn't exist.
	bool load(const std::string &path, const DeviceId &my_id, const std::string &my_name, std::string *error = nullptr);
	bool save(std::string *error = nullptr);

	json get() const;
	// Replaces the whole config (PUT /rest/config); fills defaults for missing keys.
	void replace(json cfg);
	// Applies fn to the config under the lock, then saves and notifies.
	void modify(const std::function<void(json &)> &fn);

	json folder(const std::string &id) const;   // null if absent
	json device(const DeviceId &id) const;      // null if absent
	std::vector<json> folders() const;
	std::vector<json> devices() const;
	json options() const;
	json gui() const;
	json defaults() const;

	bool folder_shared_with(const std::string &folder, const DeviceId &device) const;
	DeviceId my_id() const { return my_id_; }

	void on_change(std::function<void()> fn);

	static json default_config();
	static json default_folder();
	static json default_device();
	// Fills every key the GUI expects in a folder/device object from the defaults.
	static void fill_folder(json &f);
	static void fill_device(json &d);

private:
	void notify();
	mutable std::mutex mu_;
	json cfg_;
	std::string path_;
	DeviceId my_id_;
	std::vector<std::function<void()>> listeners_;
};

} // namespace xst
