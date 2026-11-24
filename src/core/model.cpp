#include "core/model.hpp"

#include <algorithm>
#include <random>

#include "core/puller.hpp"
#include "core/scanner.hpp"
#include "core/sha256.hpp"
#include "core/vector.hpp"

namespace xst {

namespace {

constexpr size_t kMaxBatchFiles = 1000;
constexpr size_t kMaxBatchBlocks = 5000;

std::string folder_type(const json &f) { return f.value("type", "sendreceive"); }
bool paused(const json &j) { return j.value("paused", false); }

bep::FolderType to_bep_type(const std::string &t)
{
	if (t == "sendonly")
		return bep::FolderType::send_only;
	if (t == "receiveonly")
		return bep::FolderType::receive_only;
	if (t == "receiveencrypted")
		return bep::FolderType::receive_encrypted;
	return bep::FolderType::send_receive;
}

bep::Compression to_bep_compression(const std::string &c)
{
	if (c == "never")
		return bep::Compression::never;
	if (c == "always")
		return bep::Compression::always;
	return bep::Compression::metadata;
}

} // namespace

struct Model::Folder {
	std::string id;
	json cfg;
	std::unique_ptr<fs::Filesystem> fs;
	IgnoreMatcher ignores;
	std::string ignore_error;
	FolderState state;
	std::thread thread;
	std::mutex mu;
	std::condition_variable cv;
	bool want_scan = true;
	std::vector<std::string> scan_subdirs;
	bool want_pull = true;
	std::atomic<bool> cancel{ false };
	std::chrono::steady_clock::time_point next_scan;
};

Model::Model(Config &config, Db &db, Events &events, FilesystemFactory open_fs, uint64_t short_id)
	: config_(config), db_(db), events_(events), open_fs_(std::move(open_fs)), short_id_(short_id),
	  started_(rfc3339_now())
{
}

Model::~Model() { stop(); }

void Model::start()
{
	config_.on_change([this] { reconcile_folders(); });
	reconcile_folders();
	events_.log("Starting", { { "home", "" } });
	events_.log("StartupComplete", json::object());
}

void Model::stop()
{
	if (stopping_.exchange(true))
		return;
	std::vector<std::shared_ptr<PeerConnection>> conns;
	std::map<std::string, std::unique_ptr<Folder>> folders;
	{
		std::lock_guard lock(mu_);
		for (auto &[id, p] : peers_)
			conns.push_back(p.conn);
		folders.swap(folders_);
	}
	for (auto &c : conns)
		c->close("shutting down");
	for (auto &[id, f] : folders) {
		f->cancel = true;
		f->cv.notify_all();
		if (f->thread.joinable())
			f->thread.join();
	}
}

void Model::set_state(const std::string &folder, const std::string &state, const std::string &error)
{
	std::string from;
	{
		std::lock_guard lock(mu_);
		auto it = folders_.find(folder);
		if (it == folders_.end())
			return;
		auto &s = it->second->state;
		if (s.state == state && s.error == error)
			return;
		from = s.state;
		s.state = state;
		s.error = error;
		s.state_changed = rfc3339_now();
	}
	json data = { { "folder", folder }, { "from", from }, { "to", state } };
	if (!error.empty())
		data["error"] = error;
	events_.log("StateChanged", data);
}

void Model::reconcile_folders()
{
	if (stopping_)
		return;
	std::vector<json> wanted;
	for (auto &f : config_.folders())
		if (!paused(f))
			wanted.push_back(f);
	std::vector<std::unique_ptr<Folder>> to_stop;
	std::vector<std::string> to_start;
	{
		std::lock_guard lock(mu_);
		for (auto it = folders_.begin(); it != folders_.end();) {
			const auto match = std::find_if(wanted.begin(), wanted.end(),
				[&](const json &w) { return w["id"] == it->first; });
			// Restart a folder whose path or type changed; otherwise just take the new config.
			if (match == wanted.end() || (*match)["path"] != it->second->cfg["path"] ||
				(*match)["type"] != it->second->cfg["type"]) {
				to_stop.push_back(std::move(it->second));
				it = folders_.erase(it);
			} else {
				it->second->cfg = *match;
				++it;
			}
		}
		for (auto &w : wanted) {
			const std::string id = w["id"];
			if (folders_.count(id))
				continue;
			auto f = std::make_unique<Folder>();
			f->id = id;
			f->cfg = w;
			f->state.state = "idle";
			f->state.state_changed = rfc3339_now();
			folders_[id] = std::move(f);
			to_start.push_back(id);
		}
	}
	for (auto &f : to_stop) {
		f->cancel = true;
		f->cv.notify_all();
		if (f->thread.joinable())
			f->thread.join();
	}
	for (const auto &id : to_start) {
		std::lock_guard lock(mu_);
		folders_[id]->thread = std::thread([this, id] { folder_loop(id); });
	}
	// Peers learn of folder changes through a new ClusterConfig.
	std::vector<DeviceId> devices;
	{
		std::lock_guard lock(mu_);
		for (auto &[id, p] : peers_)
			devices.push_back(id);
	}
	for (const auto &d : devices) {
		std::lock_guard lock(mu_);
		auto it = peers_.find(d);
		if (it != peers_.end())
			send_cluster_config(it->second);
	}
}

fs::Filesystem *Model::folder_fs(const std::string &folder)
{
	std::lock_guard lock(mu_);
	auto it = folders_.find(folder);
	return it == folders_.end() ? nullptr : it->second->fs.get();
}

void Model::wake(const std::string &folder, bool scan, bool pull)
{
	std::lock_guard lock(mu_);
	auto it = folders_.find(folder);
	if (it == folders_.end())
		return;
	{
		std::lock_guard fl(it->second->mu);
		it->second->want_scan |= scan;
		it->second->want_pull |= pull;
	}
	it->second->cv.notify_all();
}

void Model::scan(const std::string &folder, const std::string &subdir)
{
	{
		std::lock_guard lock(mu_);
		auto it = folders_.find(folder);
		if (it == folders_.end())
			return;
		std::lock_guard fl(it->second->mu);
		if (!subdir.empty())
			it->second->scan_subdirs.push_back(subdir);
		it->second->want_scan = true;
		it->second->cv.notify_all();
	}
}

void Model::scan_all()
{
	for (auto &f : config_.folders())
		scan(f["id"]);
}

void Model::folder_loop(const std::string &id)
{
	Folder *f;
	{
		std::lock_guard lock(mu_);
		f = folders_.at(id).get();
	}
	const std::string path = f->cfg.value("path", "");
	f->fs = open_fs_(path);
	if (!f->fs) {
		set_state(id, "error", "folder path missing or unusable: " + path);
		return;
	}
	if (!f->fs->stat("")) {
		f->fs->mkdir_all("");
		if (!f->fs->stat("")) {
			set_state(id, "error", "folder path missing: " + path);
			return;
		}
	}
	// The folder marker, as upstream creates it.
	const std::string marker = f->cfg.value("markerName", ".stfolder");
	if (!f->fs->stat(marker))
		f->fs->mkdir(marker);

	const auto rules = f->fs->rules();
	while (!f->cancel) {
		bool do_scan, do_pull;
		std::vector<std::string> subdirs;
		{
			std::unique_lock lock(f->mu);
			const int interval = f->cfg.value("rescanIntervalS", 3600);
			const auto deadline = interval > 0 ? f->next_scan : std::chrono::steady_clock::time_point::max();
			f->cv.wait_until(lock, deadline, [&] { return f->cancel || f->want_scan || f->want_pull; });
			if (f->cancel)
				break;
			if (interval > 0 && std::chrono::steady_clock::now() >= f->next_scan)
				f->want_scan = true;
			do_scan = f->want_scan;
			do_pull = f->want_pull;
			subdirs.swap(f->scan_subdirs);
			f->want_scan = false;
			f->want_pull = false;
			if (do_scan)
				f->next_scan = std::chrono::steady_clock::now() + std::chrono::seconds(interval > 0 ? interval : 3600);
		}
		const std::string type = folder_type(f->cfg);

		if (do_scan) {
			set_state(id, "scanning");
			std::string error;
			if (!load_ignores(*f->fs, f->ignores, &error)) {
				f->ignore_error = error;
				set_state(id, "error", error);
				continue;
			}
			f->ignore_error.clear();
			ScanOptions o;
			o.folder_id = id;
			o.short_id = short_id_;
			o.ignore_permissions = f->cfg.value("ignorePerms", false) || !rules.keeps_permissions;
			o.mtime_window_ns = std::max<int64_t>(rules.mtime_window_ns,
				static_cast<int64_t>(f->cfg.value("modTimeWindowS", 0)) * 1000000000LL);
			o.cancel = &f->cancel;
			o.progress = [&](int64_t hashed) {
				std::lock_guard lock(mu_);
				f->state.scan_hashed = hashed;
			};
			ScanResult r;
			{
				std::lock_guard dl(db_mu_);
				if (subdirs.empty()) {
					r = xst::scan(*f->fs, db_, f->ignores, o);
				} else {
					for (const auto &s : subdirs) {
						auto part = xst::scan(*f->fs, db_, f->ignores, o, s);
						r.changed += part.changed;
					}
				}
			}
			{
				std::lock_guard lock(mu_);
				f->state.last_scan = rfc3339_now();
			}
			events_.log("FolderScanProgress", { { "folder", id }, { "current", r.hashed_bytes },
												   { "total", r.hashed_bytes }, { "rate", 0 } });
			if (r.changed > 0) {
				events_.log("LocalIndexUpdated", { { "folder", id }, { "items", r.changed }, { "filenames", json::array() },
													 { "sequence", db_.max_sequence(id, Db::local_device()) },
													 { "version", db_.max_sequence(id, Db::local_device()) } });
				broadcast_index_updates(id);
			}
			do_pull = true;
		}

		if (do_pull && type != "sendonly" && type != "receiveencrypted") {
			bool any = false;
			db_.each_needed(id, [&](const bep::FileInfo &) {
				any = true;
				return false;
			});
			if (any) {
				set_state(id, "syncing");
				PullOptions o;
				o.folder_id = id;
				o.short_id = short_id_;
				o.ignore_permissions = f->cfg.value("ignorePerms", false) || !rules.keeps_permissions;
				o.max_conflicts = f->cfg.value("maxConflicts", 10);
				o.max_name_bytes = rules.max_name_bytes;
				o.cancel = &f->cancel;
				o.on_item = [&](const std::string &name, const std::string &err) {
					events_.log("ItemFinished", { { "folder", id }, { "item", name }, { "type", "file" },
													 { "action", "update" },
													 { "error", err.empty() ? json(nullptr) : json(err) } });
					if (err.empty()) {
						std::lock_guard lock(mu_);
						f->state.last_file = name;
						f->state.last_file_at = rfc3339_now();
					}
				};
				BlockFetcher fetch = [&](const std::vector<DeviceId> &devices, const bep::FileInfo &file,
										 const bep::BlockInfo &block, int index) {
					return fetch_block(id, devices, file, block, index);
				};
				PullResult r;
				{
					std::lock_guard dl(db_mu_);
					r = xst::pull(*f->fs, db_, f->ignores, fetch, o);
				}
				{
					std::lock_guard lock(mu_);
					f->state.pull_errors = r.errors;
				}
				if (!r.errors.empty())
					events_.log("FolderErrors", { { "folder", id }, { "errors", folder_errors(id) } });
				if (r.recorded > 0) {
					events_.log("LocalIndexUpdated", { { "folder", id }, { "items", r.recorded },
														 { "filenames", json::array() },
														 { "sequence", db_.max_sequence(id, Db::local_device()) },
														 { "version", db_.max_sequence(id, Db::local_device()) } });
					broadcast_index_updates(id);
					// More may have become needed (e.g. a block source appeared): go round again soon.
					if (!r.errors.empty()) {
						std::lock_guard fl(f->mu);
						f->next_scan = std::min(f->next_scan, std::chrono::steady_clock::now() + std::chrono::seconds(60));
					}
				}
			}
		}
		set_state(id, "idle");
		events_.log("FolderSummary", { { "folder", id }, { "summary", folder_status(id) } });
	}
}

std::optional<bep::Bytes> Model::fetch_block(const std::string &folder, const std::vector<DeviceId> &devices,
	const bep::FileInfo &file, const bep::BlockInfo &block, int index)
{
	static std::atomic<int32_t> next_id{ 1 };
	for (const auto &d : devices) {
		std::shared_ptr<PeerConnection> conn;
		{
			std::lock_guard lock(mu_);
			auto it = peers_.find(d);
			if (it == peers_.end() || !it->second.shared.count(folder))
				continue;
			conn = it->second.conn;
		}
		bep::Request r;
		r.id = next_id++ & 0x7FFFFFFF;
		r.folder = folder;
		r.name = file.name;
		r.offset = block.offset;
		r.size = block.size;
		r.hash = block.hash;
		r.block_no = index;
		if (auto data = conn->request(r)) {
			total_in_ += static_cast<int64_t>(data->size());
			return data;
		}
	}
	return std::nullopt;
}

// --- connections ---------------------------------------------------------------------------------------------------

bool Model::accept_device(const DeviceId &device, const std::string &name, const std::string &address)
{
	const json d = config_.device(device);
	if (!d.is_null())
		return !paused(d);
	json pending = json::parse(db_.kv_get("pending-devices").value_or("{}"));
	pending[device.to_string()] = { { "name", name }, { "address", address }, { "time", rfc3339_now() } };
	db_.kv_set("pending-devices", pending.dump());
	events_.log("PendingDevicesChanged",
		{ { "added", json::array({ { { "deviceID", device.to_string() }, { "name", name }, { "address", address } } }) } });
	events_.log("DeviceRejected", { { "device", device.to_string() }, { "name", name }, { "address", address } });
	return false;
}

void Model::connected(std::shared_ptr<PeerConnection> conn, const std::string &device_name, const std::string &client,
	const std::string &version)
{
	const DeviceId id = conn->device();
	std::shared_ptr<PeerConnection> old;
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(id);
		if (it != peers_.end())
			old = it->second.conn;
		Peer &p = peers_[id];
		p = Peer{};
		p.conn = conn;
		p.name = device_name;
		p.client = client;
		p.version = version;
		device_stats_[id].last_seen = rfc3339_now();
		send_cluster_config(p);
	}
	if (old)
		old->close("replaced by a new connection");
	events_.log("DeviceConnected", { { "id", id.to_string() }, { "deviceName", device_name }, { "clientName", client },
									   { "clientVersion", version }, { "addr", conn->address() }, { "type", conn->type() } });
}

void Model::disconnected(const DeviceId &device, const std::string &error)
{
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it == peers_.end())
			return;
		total_in_ += 0;
		peers_.erase(it);
		device_stats_[device].last_seen = rfc3339_now();
	}
	events_.log("DeviceDisconnected", { { "id", device.to_string() }, { "error", error } });
}

bool Model::is_connected(const DeviceId &device) const
{
	std::lock_guard lock(mu_);
	return peers_.count(device) > 0;
}

std::vector<std::shared_ptr<PeerConnection>> Model::connections() const
{
	std::lock_guard lock(mu_);
	std::vector<std::shared_ptr<PeerConnection>> out;
	for (auto &[id, p] : peers_)
		out.push_back(p.conn);
	return out;
}

void Model::send_cluster_config(Peer &peer)
{
	// Called with mu_ held.
	const DeviceId remote = peer.conn->device();
	const DeviceId me = config_.my_id();
	bep::ClusterConfig cc;
	for (const auto &f : config_.folders()) {
		if (!config_.folder_shared_with(f["id"], remote))
			continue;
		bep::Folder bf;
		bf.id = f["id"];
		bf.label = f.value("label", "");
		bf.type = to_bep_type(folder_type(f));
		bf.stop_reason = paused(f) ? bep::FolderStopReason::paused : bep::FolderStopReason::running;
		for (const auto &fd : f["devices"]) {
			const auto did = DeviceId::parse(fd.value("deviceID", ""));
			if (!did)
				continue;
			const json dcfg = config_.device(*did);
			bep::Device d;
			d.id.assign(did->bytes().begin(), did->bytes().end());
			d.name = dcfg.is_null() ? "" : dcfg.value("name", "");
			if (!dcfg.is_null())
				for (const auto &a : dcfg["addresses"])
					d.addresses.push_back(a);
			d.compression = to_bep_compression(dcfg.is_null() ? "metadata" : dcfg.value("compression", "metadata"));
			d.introducer = !dcfg.is_null() && dcfg.value("introducer", false);
			if (*did == me) {
				d.index_id = db_.local_index_id(bf.id);
				d.max_sequence = db_.max_sequence(bf.id, Db::local_device());
			} else {
				d.index_id = db_.index_id(bf.id, *did);
				d.max_sequence = db_.max_sequence(bf.id, *did);
			}
			bf.devices.push_back(std::move(d));
		}
		cc.folders.push_back(std::move(bf));
	}
	peer.conn->send(cc);
}

void Model::handle(const DeviceId &device, const bep::Message &message)
{
	if (const auto *cc = std::get_if<bep::ClusterConfig>(&message)) {
		handle_cluster_config(device, *cc);
	} else if (const auto *idx = std::get_if<bep::Index>(&message)) {
		handle_index(device, idx->folder, idx->files, true);
	} else if (const auto *iu = std::get_if<bep::IndexUpdate>(&message)) {
		handle_index(device, iu->folder, iu->files, false);
	} else if (const auto *rq = std::get_if<bep::Request>(&message)) {
		handle_request(device, *rq);
	} else if (const auto *cl = std::get_if<bep::Close>(&message)) {
		std::shared_ptr<PeerConnection> conn;
		{
			std::lock_guard lock(mu_);
			auto it = peers_.find(device);
			if (it != peers_.end())
				conn = it->second.conn;
		}
		if (conn)
			conn->close("peer closed: " + cl->reason);
	}
	// Ping, DownloadProgress: nothing to do. Responses are routed by the connection.
}

void Model::handle_cluster_config(const DeviceId &device, const bep::ClusterConfig &cc)
{
	const DeviceId me = config_.my_id();
	std::vector<std::pair<std::string, bool>> to_send; // folder, full index
	json pending = json::parse(db_.kv_get("pending-folders").value_or("{}"));
	bool pending_changed = false;
	std::string device_name;
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it == peers_.end())
			return;
		device_name = it->second.name;
	}
	std::set<std::string> shared;
	for (const auto &f : cc.folders) {
		const json fcfg = config_.folder(f.id);
		if (fcfg.is_null() || !config_.folder_shared_with(f.id, device)) {
			// Offered but not shared with them here: pending, unless ignored.
			bool ignored = false;
			const json dcfg = config_.device(device);
			if (!dcfg.is_null())
				for (const auto &ig : dcfg["ignoredFolders"])
					if (ig.value("id", "") == f.id)
						ignored = true;
			if (!ignored && f.type != bep::FolderType::receive_encrypted) {
				auto &entry = pending[f.id][device.to_string()];
				if (entry.is_null() || entry.value("label", "") != f.label) {
					entry = { { "time", rfc3339_now() }, { "label", f.label },
						{ "receiveEncrypted", f.type == bep::FolderType::receive_encrypted },
						{ "remoteEncrypted", false } };
					pending_changed = true;
					events_.log("PendingFoldersChanged",
						{ { "added", json::array({ { { "deviceID", device.to_string() }, { "folderID", f.id },
												   { "folderLabel", f.label } } }) } });
					events_.log("FolderRejected",
						{ { "device", device.to_string() }, { "folder", f.id }, { "folderLabel", f.label } });
				}
			}
			continue;
		}
		if (paused(fcfg) || f.stop_reason == bep::FolderStopReason::paused)
			continue;
		shared.insert(f.id);
		// Their view of us: delta indexes if they have our current index ID.
		bool full = true;
		int64_t start = 0;
		for (const auto &d : f.devices) {
			const auto did = DeviceId::from_bytes(d.id);
			if (!did)
				continue;
			if (*did == me) {
				if (d.index_id == db_.local_index_id(f.id) && d.max_sequence <= db_.max_sequence(f.id, Db::local_device())) {
					start = d.max_sequence;
					full = start == 0;
				}
			} else if (*did == device) {
				// Their description of themselves: a new index ID means their database was reset.
				std::lock_guard dl(db_mu_);
				if (d.index_id == 0 || d.index_id != db_.index_id(f.id, device)) {
					db_.begin();
					db_.drop_device(f.id, device);
					if (d.index_id != 0)
						db_.set_index_id(f.id, device, d.index_id);
					db_.commit();
				}
			}
		}
		{
			std::lock_guard lock(mu_);
			auto it = peers_.find(device);
			if (it != peers_.end())
				it->second.sent[f.id] = start;
		}
		to_send.emplace_back(f.id, full);
	}
	// Folders no longer pending from this device.
	for (auto it = pending.begin(); it != pending.end(); ++it) {
		if (!it.value().contains(device.to_string()))
			continue;
		const bool still = std::any_of(cc.folders.begin(), cc.folders.end(), [&](const bep::Folder &f) {
			return f.id == it.key() && !config_.folder_shared_with(f.id, device);
		});
		if (!still) {
			it.value().erase(device.to_string());
			pending_changed = true;
		}
	}
	if (pending_changed) {
		for (auto it = pending.begin(); it != pending.end();)
			it = it.value().empty() ? pending.erase(it) : std::next(it);
		db_.kv_set("pending-folders", pending.dump());
	}
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it == peers_.end())
			return;
		it->second.shared = shared;
		it->second.got_cluster_config = true;
	}
	events_.log("ClusterConfigReceived", { { "device", device.to_string() } });
	for (const auto &[folder, full] : to_send) {
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it != peers_.end())
			send_indexes(it->second, folder, full);
	}
}

void Model::send_indexes(Peer &peer, const std::string &folder, bool full)
{
	// Called with mu_ held. Batches of up to 1000 files / 5000 blocks; the first full one is an Index.
	int64_t after = peer.sent[folder];
	bool first = full;
	std::vector<bep::FileInfo> batch;
	size_t blocks = 0;
	auto flush = [&] {
		if (batch.empty() && !first)
			return;
		const int64_t last = batch.empty() ? after : batch.back().sequence;
		if (first) {
			peer.conn->send(bep::Index{ folder, batch, last });
			first = false;
		} else {
			peer.conn->send(bep::IndexUpdate{ folder, batch, last, after });
		}
		after = last;
		batch.clear();
		blocks = 0;
	};
	db_.local_since(folder, after, [&](const bep::FileInfo &f) {
		bep::FileInfo w = f;
		w.invalid = is_invalid(f);
		// Receive-only local changes must never win elsewhere (lib/model prepareFileInfoForIndex).
		if (f.local_flags & kFlagReceiveOnly)
			w.version = {};
		w.local_flags = 0;
		blocks += w.blocks.size();
		batch.push_back(std::move(w));
		if (batch.size() >= kMaxBatchFiles || blocks >= kMaxBatchBlocks)
			flush();
		return true;
	});
	flush();
	peer.sent[folder] = after;
}

void Model::broadcast_index_updates(const std::string &folder)
{
	std::lock_guard lock(mu_);
	for (auto &[id, p] : peers_)
		if (p.shared.count(folder))
			send_indexes(p, folder, false);
}

void Model::handle_index(const DeviceId &device, const std::string &folder, const std::vector<bep::FileInfo> &files,
	bool full)
{
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it == peers_.end() || !it->second.shared.count(folder))
			return;
	}
	{
		std::lock_guard dl(db_mu_);
		db_.begin();
		if (full)
			db_.drop_device(folder, device);
		std::vector<bep::FileInfo> in;
		in.reserve(files.size());
		for (auto f : files) {
			f.local_flags = f.invalid ? static_cast<uint32_t>(kFlagRemoteInvalid) : 0u;
			in.push_back(std::move(f));
		}
		db_.update(folder, device, in);
		db_.commit();
	}
	events_.log("RemoteIndexUpdated", { { "device", device.to_string() }, { "folder", folder }, { "items", files.size() },
										  { "sequence", db_.max_sequence(folder, device) } });
	wake(folder, false, true);
}

void Model::handle_request(const DeviceId &device, const bep::Request &req)
{
	std::shared_ptr<PeerConnection> conn;
	{
		std::lock_guard lock(mu_);
		auto it = peers_.find(device);
		if (it == peers_.end())
			return;
		conn = it->second.conn;
	}
	bep::Response resp;
	resp.id = req.id;
	auto reply = [&](bep::ErrorCode code) {
		resp.code = code;
		conn->send(resp);
	};
	const json f = config_.folder(req.folder);
	if (f.is_null() || !config_.folder_shared_with(req.folder, device) || paused(f))
		return reply(bep::ErrorCode::generic);
	if (req.size < 0 || req.offset < 0 || req.size > 32 * 1024 * 1024 || req.name.empty() ||
		req.name.find("..") != std::string::npos || req.name.front() == '/')
		return reply(bep::ErrorCode::invalid_file);
	fs::Filesystem *fsys = folder_fs(req.folder);
	if (!fsys)
		return reply(bep::ErrorCode::generic);
	if (IgnoreMatcher::is_internal(req.name))
		return reply(bep::ErrorCode::invalid_file);
	{
		std::lock_guard lock(mu_);
		auto it = folders_.find(req.folder);
		if (it != folders_.end() && it->second->ignores.match(req.name).ignored)
			return reply(bep::ErrorCode::invalid_file);
	}
	auto st = fsys->stat(req.name);
	if (!st || st->kind != fs::Kind::file)
		return reply(bep::ErrorCode::no_such_file);
	auto file = fsys->open_read(req.name);
	if (!file)
		return reply(bep::ErrorCode::no_such_file);
	resp.data.resize(static_cast<size_t>(req.size));
	const long n = file->read_at(resp.data, req.offset);
	if (n < 0)
		return reply(bep::ErrorCode::generic);
	resp.data.resize(static_cast<size_t>(n));
	if (!req.hash.empty() && req.hash.size() == 32) {
		const auto h = sha256(resp.data);
		if (!std::equal(h.begin(), h.end(), req.hash.begin())) {
			// Changed on disk since we indexed it: rescan it, and say so.
			scan(req.folder, std::string(fs::parent(req.name)));
			resp.data.clear();
			return reply(bep::ErrorCode::no_such_file);
		}
	}
	total_out_ += n;
	conn->send(resp);
}

// --- status for the API and UI ---------------------------------------------------------------------------------------

Model::FolderState Model::folder_state(const std::string &folder) const
{
	std::lock_guard lock(mu_);
	auto it = folders_.find(folder);
	if (it == folders_.end()) {
		FolderState s;
		const json f = config_.folder(folder);
		s.state = f.is_null() ? "unknown" : (paused(f) ? "paused" : "unknown");
		return s;
	}
	return it->second->state;
}

json Model::folder_status(const std::string &folder)
{
	const auto global = db_.global_counts(folder);
	const auto local = db_.local_counts(folder);
	const auto need = db_.need_counts(folder);
	const auto st = folder_state(folder);
	json remote = json::object();
	for (const auto &d : config_.devices()) {
		const auto id = DeviceId::parse(d.value("deviceID", ""));
		if (id && *id != config_.my_id() && config_.folder_shared_with(folder, *id))
			remote[id->to_string()] = db_.max_sequence(folder, *id);
	}
	const int64_t seq = db_.max_sequence(folder, Db::local_device());
	const auto items = [](const Db::Counts &c) { return c.files + c.directories + c.deleted; };
	return {
		{ "errors", st.pull_errors.size() }, { "pullErrors", st.pull_errors.size() }, { "invalid", "" },
		{ "globalFiles", global.files }, { "globalDirectories", global.directories }, { "globalSymlinks", 0 },
		{ "globalDeleted", global.deleted }, { "globalBytes", global.bytes }, { "globalTotalItems", items(global) },
		{ "localFiles", local.files }, { "localDirectories", local.directories }, { "localSymlinks", 0 },
		{ "localDeleted", local.deleted }, { "localBytes", local.bytes }, { "localTotalItems", items(local) },
		{ "needFiles", need.files }, { "needDirectories", need.directories }, { "needSymlinks", 0 },
		{ "needDeletes", need.deleted }, { "needBytes", need.bytes }, { "needTotalItems", items(need) },
		{ "receiveOnlyChangedFiles", 0 }, { "receiveOnlyChangedDirectories", 0 }, { "receiveOnlyChangedSymlinks", 0 },
		{ "receiveOnlyChangedDeletes", 0 }, { "receiveOnlyChangedBytes", 0 }, { "receiveOnlyTotalItems", 0 },
		{ "inSyncFiles", global.files - need.files }, { "inSyncBytes", global.bytes - need.bytes },
		{ "state", st.state }, { "stateChanged", st.state_changed.empty() ? started_ : st.state_changed },
		{ "error", st.error }, { "version", seq }, { "sequence", seq }, { "remoteSequence", remote },
		{ "ignorePatterns", false }, { "watchError", "" },
	};
}

json Model::completion(const std::string &folder, const DeviceId &device)
{
	auto one = [&](const std::string &fid, Db::Counts &g_out, Db::Counts &n_out) {
		const auto g = db_.global_counts(fid);
		const auto n = (device == config_.my_id() || device == Db::local_device()) ? db_.need_counts(fid)
																					: db_.remote_need_counts(fid, device);
		g_out.files += g.files + g.directories;
		g_out.bytes += g.bytes;
		n_out.files += n.files + n.directories;
		n_out.deleted += n.deleted;
		n_out.bytes += n.bytes;
	};
	Db::Counts g, n;
	int64_t seq = 0;
	if (!folder.empty()) {
		one(folder, g, n);
		seq = db_.max_sequence(folder, device == config_.my_id() ? Db::local_device() : device);
	} else {
		for (const auto &f : config_.folders())
			if (!paused(f) && (device == config_.my_id() || config_.folder_shared_with(f["id"], device)))
				one(f["id"], g, n);
	}
	double pct = g.bytes == 0 ? 100.0 : 100.0 * (1.0 - static_cast<double>(n.bytes) / static_cast<double>(g.bytes));
	if (n.bytes == 0 && n.deleted > 0)
		pct = 95;
	std::string remote_state = "unknown";
	if (device != config_.my_id() && is_connected(device))
		remote_state = "valid";
	return { { "completion", pct }, { "globalBytes", g.bytes }, { "needBytes", n.bytes }, { "globalItems", g.files },
		{ "needItems", n.files }, { "needDeletes", n.deleted }, { "sequence", seq }, { "remoteState", remote_state } };
}

json Model::folder_errors(const std::string &folder) const
{
	json out = json::array();
	std::lock_guard lock(mu_);
	auto it = folders_.find(folder);
	if (it == folders_.end())
		return out;
	for (const auto &[path, err] : it->second->state.pull_errors)
		out.push_back({ { "path", path }, { "error", err } });
	return out;
}

std::vector<std::string> Model::ignores(const std::string &folder, std::string *error)
{
	fs::Filesystem *fsys = folder_fs(folder);
	if (!fsys)
		return {};
	IgnoreMatcher m;
	load_ignores(*fsys, m, error);
	// The raw lines as in the file.
	std::vector<std::string> lines;
	if (auto f = fsys->open_read(".stignore")) {
		auto st = fsys->stat(".stignore");
		std::string text(static_cast<size_t>(st ? st->size : 0), '\0');
		f->read_at(std::span(reinterpret_cast<uint8_t *>(text.data()), text.size()), 0);
		size_t pos = 0;
		while (pos < text.size()) {
			auto end = text.find('\n', pos);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(line);
			pos = end + 1;
		}
	}
	return lines;
}

bool Model::set_ignores(const std::string &folder, const std::vector<std::string> &lines)
{
	fs::Filesystem *fsys = folder_fs(folder);
	if (!fsys)
		return false;
	if (lines.empty()) {
		fsys->remove(".stignore");
	} else {
		std::string text;
		for (const auto &l : lines)
			text += l + "\n";
		const std::string tmp = ".stignore.tmp";
		auto f = fsys->create(tmp);
		if (!f)
			return false;
		f->write_at(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), 0);
		f->close();
		if (!fsys->rename(tmp, ".stignore"))
			return false;
	}
	scan(folder);
	return true;
}

json Model::pending_devices() const
{
	json p = json::parse(const_cast<Db &>(db_).kv_get("pending-devices").value_or("{}"));
	// Drop entries for devices that have since been added.
	for (auto it = p.begin(); it != p.end();) {
		const auto id = DeviceId::parse(it.key());
		it = (id && !config_.device(*id).is_null()) ? p.erase(it) : std::next(it);
	}
	return p;
}

json Model::pending_folders(const DeviceId &device) const
{
	json p = json::parse(const_cast<Db &>(db_).kv_get("pending-folders").value_or("{}"));
	json out = json::object();
	for (auto it = p.begin(); it != p.end(); ++it) {
		json offered = json::object();
		for (auto d = it.value().begin(); d != it.value().end(); ++d) {
			if (!device.empty() && DeviceId::parse(d.key()) != device)
				continue;
			const auto did = DeviceId::parse(d.key());
			if (did && config_.folder_shared_with(it.key(), *did))
				continue;
			offered[d.key()] = d.value();
		}
		if (!offered.empty())
			out[it.key()] = { { "offeredBy", offered } };
	}
	return out;
}

void Model::dismiss_pending_device(const DeviceId &device)
{
	json p = json::parse(db_.kv_get("pending-devices").value_or("{}"));
	p.erase(device.to_string());
	db_.kv_set("pending-devices", p.dump());
	events_.log("PendingDevicesChanged", { { "removed", json::array({ { { "deviceID", device.to_string() } } }) } });
}

void Model::dismiss_pending_folder(const std::string &folder, const DeviceId &device)
{
	json p = json::parse(db_.kv_get("pending-folders").value_or("{}"));
	if (p.contains(folder)) {
		if (device.empty())
			p.erase(folder);
		else
			p[folder].erase(device.to_string());
		if (p.contains(folder) && p[folder].empty())
			p.erase(folder);
	}
	db_.kv_set("pending-folders", p.dump());
	events_.log("PendingFoldersChanged", { { "removed", json::array({ { { "folderID", folder } } }) } });
}

json Model::device_stats() const
{
	std::lock_guard lock(mu_);
	json out = json::object();
	for (const auto &d : config_.devices()) {
		const auto id = DeviceId::parse(d.value("deviceID", ""));
		if (!id)
			continue;
		auto it = device_stats_.find(*id);
		const bool connected = peers_.count(*id) > 0;
		out[id->to_string()] = { { "lastSeen", connected ? rfc3339_now()
														 : (it != device_stats_.end() ? it->second.last_seen
																					  : "1970-01-01T00:00:00Z") },
			{ "lastConnectionDurationS", 0 } };
	}
	return out;
}

json Model::folder_stats() const
{
	std::lock_guard lock(mu_);
	json out = json::object();
	for (const auto &[id, f] : folders_)
		out[id] = { { "lastFile", { { "at", f->state.last_file_at.empty() ? "1970-01-01T00:00:00Z" : f->state.last_file_at },
								 { "filename", f->state.last_file }, { "deleted", f->state.last_file_deleted } } },
			{ "lastScan", f->state.last_scan.empty() ? "1970-01-01T00:00:00Z" : f->state.last_scan } };
	return out;
}

json Model::need(const std::string &folder, int page, int per_page)
{
	json progress = json::array(), queued = json::array(), rest = json::array();
	int i = 0;
	const int skip = (page - 1) * per_page;
	db_.each_needed(folder, [&](const bep::FileInfo &f) {
		if (i >= skip && i < skip + per_page)
			rest.push_back({ { "name", f.name }, { "size", f.size }, { "type", f.type == bep::FileInfoType::directory ? "FILE_INFO_TYPE_DIRECTORY" : "FILE_INFO_TYPE_FILE" },
				{ "modified", rfc3339(f.modified_s, f.modified_ns) }, { "deleted", f.deleted } });
		i++;
		return true;
	});
	return { { "progress", progress }, { "queued", queued }, { "rest", rest }, { "page", page }, { "perpage", per_page },
		{ "total", i } };
}

json Model::browse(const std::string &folder, const std::string &prefix, int levels)
{
	json out = json::object();
	db_.each_local(folder, [&](const bep::FileInfo &f) {
		if (f.deleted || is_invalid(f))
			return true;
		if (!prefix.empty() && !f.name.starts_with(prefix + "/"))
			return true;
		std::string rel = prefix.empty() ? f.name : f.name.substr(prefix.size() + 1);
		json *node = &out;
		size_t pos = 0;
		int depth = 0;
		for (;;) {
			const auto slash = rel.find('/', pos);
			const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
			if (slash == std::string::npos) {
				if (f.type == bep::FileInfoType::directory) {
					if (!node->contains(part))
						(*node)[part] = json::object();
				} else {
					(*node)[part] = json::array({ rfc3339(f.modified_s, f.modified_ns), f.size });
				}
				break;
			}
			if (levels >= 0 && depth >= levels)
				break;
			if (!node->contains(part) || !(*node)[part].is_object())
				(*node)[part] = json::object();
			node = &(*node)[part];
			pos = slash + 1;
			depth++;
		}
		return true;
	});
	return out;
}

json Model::file_info(const std::string &folder, const std::string &name)
{
	auto to_json = [](const bep::FileInfo &f) {
		json v = json::array();
		for (const auto &c : f.version.counters)
			v.push_back(std::to_string(c.id) + ":" + std::to_string(c.value));
		return json{ { "name", f.name }, { "size", f.size }, { "modified", rfc3339(f.modified_s, f.modified_ns) },
			{ "deleted", f.deleted }, { "invalid", is_invalid(f) }, { "sequence", f.sequence },
			{ "numBlocks", f.blocks.size() }, { "version", v }, { "localFlags", f.local_flags },
			{ "type", f.type == bep::FileInfoType::directory ? "FILE_INFO_TYPE_DIRECTORY" : "FILE_INFO_TYPE_FILE" },
			{ "permissions", "0" + std::to_string(f.permissions) }, { "noPermissions", f.no_permissions } };
	};
	json out = json::object();
	if (auto g = db_.global(folder, name))
		out["global"] = to_json(*g);
	if (auto l = db_.get(folder, Db::local_device(), name))
		out["local"] = to_json(*l);
	json avail = json::array();
	for (const auto &d : db_.availability(folder, name))
		avail.push_back({ { "id", d.to_string() }, { "fromTemporary", false } });
	out["availability"] = avail;
	return out;
}

void Model::override_folder(const std::string &folder)
{
	// Send-only: make our versions win by bumping them past the global ones.
	std::lock_guard dl(db_mu_);
	db_.begin();
	std::vector<bep::FileInfo> changed;
	db_.each_needed(folder, [&](const bep::FileInfo &g) {
		auto local = db_.get(folder, Db::local_device(), g.name);
		bep::FileInfo f;
		if (local) {
			f = *local;
		} else {
			f.name = g.name;
			f.type = g.type;
			f.deleted = true;
		}
		f.version = vec::update(vec::merge(f.version, g.version), short_id_);
		f.sequence = db_.next_local_sequence(folder);
		changed.push_back(f);
		return true;
	});
	db_.update(folder, Db::local_device(), changed);
	db_.commit();
	broadcast_index_updates(folder);
}

void Model::revert_folder(const std::string &folder)
{
	// Receive-only: forget local changes so the global versions are pulled again.
	std::lock_guard dl(db_mu_);
	db_.begin();
	std::vector<bep::FileInfo> changed;
	db_.each_local(folder, [&](const bep::FileInfo &f) {
		if (f.local_flags & kFlagReceiveOnly) {
			bep::FileInfo r = f;
			r.version = {};
			r.local_flags = kFlagMustRescan;
			r.sequence = db_.next_local_sequence(folder);
			changed.push_back(r);
		}
		return true;
	});
	db_.update(folder, Db::local_device(), changed);
	db_.commit();
	wake(folder, false, true);
}

} // namespace xst
