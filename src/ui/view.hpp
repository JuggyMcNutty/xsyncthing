#pragma once

// The console UI's view of the daemon: a snapshot taken from the same in-process Model and Config the REST API uses,
// plus the actions the UI can take. Nothing here keeps state of its own (step 24), so the UI and the Web GUI always
// agree. Portable: the host build runs it in tests.
#include <cstdint>
#include <string>
#include <vector>

#include "app.hpp"

namespace xst::ui {

struct FolderRow {
	std::string id, label, path, type, state, error;
	double completion = 100; // local
	int64_t global_bytes = 0, need_bytes = 0, need_items = 0;
	int64_t local_files = 0;
	int errors = 0;
	bool paused = false;
	std::vector<std::string> shared_with; // device names
	int64_t scan_files = 0;            // while scanning
	int64_t pull_done = 0, pull_total = 0; // while syncing
};

struct DeviceRow {
	std::string id, short_id, name, address, type, client;
	bool connected = false, paused = false;
	double completion = 100; // their completion of what we share
};

struct PendingDevice {
	std::string id, name, address;
};

struct PendingFolder {
	std::string folder_id, label, device_id, device_name;
};

struct Snapshot {
	std::string device_id, device_name, version;
	std::string address;      // this console's LAN address
	std::string gui_url;      // where the Web GUI listens, "" if off
	std::string api_key;
	bool gui_has_password = false;
	std::string gui_user;
	std::vector<FolderRow> folders;
	std::vector<DeviceRow> devices;
	std::vector<PendingDevice> pending_devices;
	std::vector<PendingFolder> pending_folders;
	std::vector<std::string> system_errors;
	std::string relay;        // joined relay, if any
	std::vector<std::string> discovery_errors;
	bool local_discovery = true, global_discovery = true, relays = true;
	std::string listen_address;
};

// Reads everything the screens show, from the model and config.
Snapshot snapshot(App &app);

// The UI's actions, all through Config/Model (the same calls the REST API makes).
bool add_device(App &app, const std::string &id, const std::string &name, std::string *error);
bool accept_pending_device(App &app, const std::string &id, const std::string &name);
void dismiss_pending_device(App &app, const std::string &id);
// Creates the folder (if new) at path, shared with device; or adds the device to an existing folder.
bool share_folder(App &app, const std::string &folder_id, const std::string &label, const std::string &path,
	const std::string &device_id, std::string *error);
bool accept_pending_folder(App &app, const PendingFolder &p, const std::string &path, std::string *error);
void dismiss_pending_folder(App &app, const PendingFolder &p);
void set_folder_paused(App &app, const std::string &folder_id, bool paused);
void set_device_paused(App &app, const std::string &device_id, bool paused);
void rescan(App &app, const std::string &folder_id);
void remove_folder(App &app, const std::string &folder_id);
void remove_device(App &app, const std::string &device_id);
void set_option(App &app, const std::string &key, const nlohmann::json &value);
void set_gui_enabled(App &app, bool enabled);
void set_gui_password(App &app, const std::string &user, const std::string &password);

// Where a new folder goes by default (defaults.folder.path + label), with names cut to the volume's limit.
std::string default_folder_path(App &app, const std::string &label);
// "1.5 MiB", "830 B"...
std::string human_bytes(int64_t bytes);

} // namespace xst::ui
