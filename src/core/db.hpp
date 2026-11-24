#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/bep.hpp"
#include "core/device_id.hpp"

struct sqlite3;

namespace xst {

// Local flags, as lib/protocol (FileInfo.local_flags, never sent).
enum LocalFlag : uint32_t {
	kFlagUnsupported = 1 << 0,
	kFlagIgnored = 1 << 1,
	kFlagMustRescan = 1 << 2,
	kFlagReceiveOnly = 1 << 3,
	kFlagRemoteInvalid = 1 << 6,
};
inline constexpr uint32_t kLocalInvalidFlags =
	kFlagUnsupported | kFlagIgnored | kFlagMustRescan | kFlagReceiveOnly | kFlagRemoteInvalid;

inline bool is_invalid(const bep::FileInfo &f) { return f.invalid || (f.local_flags & kLocalInvalidFlags) != 0; }

// The index database: every device's view of every shared folder, one row per (folder, device, name), with the file
// stored as its protobuf encoding. The local device is DeviceId with all bytes 0xFF (Syncthing's LocalDeviceID).
// The global version of a name is computed from all devices' rows: the greatest version vector wins, and concurrent
// ones are ordered as Syncthing orders conflicts (valid over invalid, newer mtime, then the vector's device ID).
class Db {
public:
	static std::unique_ptr<Db> open(const std::string &path, std::string *error = nullptr);
	~Db();

	static DeviceId local_device();

	// Folder bookkeeping.
	uint64_t index_id(const std::string &folder, const DeviceId &device);
	void set_index_id(const std::string &folder, const DeviceId &device, uint64_t id);
	// Ours, made on first use as a random non-zero 64-bit number.
	uint64_t local_index_id(const std::string &folder);
	int64_t max_sequence(const std::string &folder, const DeviceId &device);

	// Replace or add files for a device. For the local device the caller assigns sequences (next_local_sequence).
	void update(const std::string &folder, const DeviceId &device, const std::vector<bep::FileInfo> &files);
	void drop_device(const std::string &folder, const DeviceId &device);
	void drop_folder(const std::string &folder);
	int64_t next_local_sequence(const std::string &folder);

	std::optional<bep::FileInfo> get(const std::string &folder, const DeviceId &device, const std::string &name);
	std::optional<bep::FileInfo> global(const std::string &folder, const std::string &name);
	// Devices whose version of name equals the global one and is valid: where the content can be fetched.
	std::vector<DeviceId> availability(const std::string &folder, const std::string &name);

	void each_local(const std::string &folder, const std::function<bool(const bep::FileInfo &)> &fn);
	void each_global(const std::string &folder, const std::function<bool(const bep::FileInfo &)> &fn);
	// Local files with sequence > after, in sequence order: what to send a peer in an index update.
	void local_since(const std::string &folder, int64_t after, const std::function<bool(const bep::FileInfo &)> &fn);
	// Names where the global version is newer than ours (or we lack it), with the global file: what to pull. Reads
	// in pages of `page` rows, so a folder with many needed files doesn't sit in memory at once.
	void each_needed(const std::string &folder, const std::function<bool(const bep::FileInfo &)> &fn,
		size_t page = 1000);
	// A local block with this hash, to copy instead of fetching: (file name, offset, size).
	struct BlockRef {
		std::string name;
		int64_t offset = 0;
		int32_t size = 0;
	};
	std::optional<BlockRef> find_local_block(const std::string &folder, std::span<const uint8_t> hash);
	// Global files the device lacks or has older (deletions only where it still has the file).
	void each_remote_needed(const std::string &folder, const DeviceId &device,
		const std::function<bool(const bep::FileInfo &)> &fn);
	// What a remote device needs from the global set (for completion figures).
	struct Counts {
		int64_t files = 0, directories = 0, deleted = 0, bytes = 0;
	};
	Counts local_counts(const std::string &folder);
	Counts global_counts(const std::string &folder);
	Counts need_counts(const std::string &folder);
	Counts remote_need_counts(const std::string &folder, const DeviceId &device);

	// Key/value store for small settings (pending devices, stats).
	std::optional<std::string> kv_get(const std::string &key);
	void kv_set(const std::string &key, const std::string &value);

	void begin();
	void commit();

private:
	explicit Db(sqlite3 *db) : db_(db) {}
	void exec(const char *sql);
	std::vector<bep::FileInfo> versions(const std::string &folder, const std::string &name,
		std::vector<DeviceId> *devices = nullptr);
	void recompute_global(const std::string &folder, const std::string &name);
	sqlite3 *db_;
};

// Which of two versions of a file is the global one: true if a beats b (lib/protocol WinsConflict when concurrent).
bool newer_than(const bep::FileInfo &a, const bep::FileInfo &b);

} // namespace xst
