#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/bep.hpp"
#include "core/config.hpp"
#include "core/db.hpp"
#include "core/events.hpp"
#include "core/fs.hpp"
#include "core/ignore.hpp"

namespace xst {

// A peer connection as the model sees it (net/connection_service implements it over BEP).
class PeerConnection {
public:
	virtual ~PeerConnection() = default;
	virtual DeviceId device() const = 0;
	virtual bool send(const bep::Message &message) = 0;
	// Sends a Request and waits for its Response; nullopt on error, timeout, or an error code.
	virtual std::optional<bep::Bytes> request(const bep::Request &request) = 0;
	virtual void close(const std::string &reason) = 0;
	// For /rest/system/connections.
	virtual std::string address() const = 0;
	virtual std::string type() const = 0; // "tcp-client", "tcp-server", "relay-client", "relay-server"
	virtual std::string client_version() const = 0;
	virtual std::string crypto() const = 0;
	virtual bool is_local() const = 0;
	virtual int64_t in_bytes() const = 0;
	virtual int64_t out_bytes() const = 0;
	virtual std::chrono::system_clock::time_point started() const = 0;
};

// How a folder's path is opened on this platform (the console checks the drive's filesystem for the rules).
using FilesystemFactory = std::function<std::unique_ptr<fs::Filesystem>(const std::string &path)>;

// The model: folders, their scanning and pulling, and the BEP message handling for connected devices. The one
// source of truth for the REST API and the console UI.
class Model {
public:
	struct FolderState {
		std::string state = "idle"; // idle, scanning, scan-waiting, syncing, sync-waiting, error, unknown
		std::string error;
		std::string state_changed;
		int64_t scan_hashed = 0;
		int64_t scan_files = 0;   // walked so far in the running scan
		int64_t pull_done = 0;    // pulled so far in the running pull
		int64_t pull_total = 0;   // needed when the pull started
		std::map<std::string, std::string> pull_errors;
		std::string last_scan;
		int64_t sequence = 0;
		std::string last_file;
		std::string last_file_at;
		bool last_file_deleted = false;
	};
	struct DeviceStats {
		std::string last_seen; // RFC 3339
		std::string last_connection_duration_s;
	};

	Model(Config &config, Db &db, Events &events, FilesystemFactory open_fs, uint64_t short_id);
	~Model();

	void start();
	void stop();

	// Connections (from net): register after Hello; the model sends ClusterConfig and handles messages.
	void connected(std::shared_ptr<PeerConnection> conn, const std::string &device_name, const std::string &client,
		const std::string &version);
	void disconnected(const DeviceId &device, const std::string &error);
	void handle(const DeviceId &device, const bep::Message &message);
	bool is_connected(const DeviceId &device) const;
	std::vector<std::shared_ptr<PeerConnection>> connections() const;
	// Should a connection from this device be accepted? (configured and not paused); records it as pending if not.
	bool accept_device(const DeviceId &device, const std::string &name, const std::string &address);

	// Folder operations.
	void scan(const std::string &folder, const std::string &subdir = "");
	void scan_all();
	FolderState folder_state(const std::string &folder) const;
	json folder_status(const std::string &folder); // /rest/db/status
	json completion(const std::string &folder, const DeviceId &device); // /rest/db/completion
	json folder_errors(const std::string &folder) const;
	std::vector<std::string> ignores(const std::string &folder, std::string *error = nullptr);
	bool set_ignores(const std::string &folder, const std::vector<std::string> &lines);
	json pending_devices() const;
	json pending_folders(const DeviceId &device) const;
	void dismiss_pending_device(const DeviceId &device);
	void dismiss_pending_folder(const std::string &folder, const DeviceId &device);
	json device_stats() const;
	json folder_stats() const;
	json need(const std::string &folder, int page, int per_page);
	// /rest/db/browse: the global tree under prefix as [{name, modTime, size, type, children}] (lib/model TreeEntry).
	json browse(const std::string &folder, const std::string &prefix, int levels, bool dirs_only = false);
	json file_info(const std::string &folder, const std::string &name);
	// /rest/db/remoteneed: what device lacks of the global set; /rest/db/localchanged: receive-only local changes.
	json remote_need(const std::string &folder, const DeviceId &device, int page, int per_page);
	json local_changed(const std::string &folder, int page, int per_page);
	// FileInfo as /rest/db/* show it (lib/api fileIntfJSONMap).
	static json file_json(const bep::FileInfo &f);
	void override_folder(const std::string &folder);
	void revert_folder(const std::string &folder);

	// The GUI's notification area (/rest/system/error).
	json system_errors() const;
	void add_system_error(const std::string &message);
	void clear_system_errors();

	Db &db() { return db_; }
	Config &config() { return config_; }
	Events &events() { return events_; }
	uint64_t short_id() const { return short_id_; }
	std::string started_at() const { return started_; }

	// Totals for /rest/system/connections.
	int64_t total_in() const { return total_in_; }
	int64_t total_out() const { return total_out_; }

private:
	struct Folder;
	struct Peer {
		std::shared_ptr<PeerConnection> conn;
		std::string name, client, version;
		std::set<std::string> shared;         // folders both sides share
		std::map<std::string, int64_t> sent;  // folder -> last local sequence sent
		bool got_cluster_config = false;
	};

	void folder_loop(const std::string &id);
	void reconcile_folders();
	void send_cluster_config(Peer &peer);
	void send_indexes(Peer &peer, const std::string &folder, bool full);
	void broadcast_index_updates(const std::string &folder);
	void handle_cluster_config(const DeviceId &device, const bep::ClusterConfig &cc);
	void handle_index(const DeviceId &device, const std::string &folder, const std::vector<bep::FileInfo> &files,
		bool full);
	void handle_request(const DeviceId &device, const bep::Request &request);
	void set_state(const std::string &folder, const std::string &state, const std::string &error = "");
	std::optional<bep::Bytes> fetch_block(const std::string &folder, const std::vector<DeviceId> &devices,
		const bep::FileInfo &file, const bep::BlockInfo &block, int index);
	fs::Filesystem *folder_fs(const std::string &folder);
	void wake(const std::string &folder, bool scan, bool pull);
	// LocalChangeDetected / RemoteChangeDetected (the GUI's "Recent Changes"), as lib/model emitDiskChangeEvents.
	void emit_disk_changes(const std::string &folder, const std::vector<bep::FileInfo> &files, bool local);

	Config &config_;
	Db &db_;
	Events &events_;
	FilesystemFactory open_fs_;
	uint64_t short_id_;
	std::string started_;

	mutable std::mutex mu_;
	std::map<DeviceId, Peer> peers_;
	std::map<std::string, std::unique_ptr<Folder>> folders_;
	std::map<DeviceId, DeviceStats> device_stats_;
	std::atomic<bool> stopping_{ false };
	std::atomic<int64_t> total_in_{ 0 }, total_out_{ 0 };
	std::mutex db_mu_; // serializes database writes from folder threads and the network
	std::vector<std::pair<std::string, std::string>> system_errors_; // when, message
};

} // namespace xst
