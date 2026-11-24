#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/bep.hpp"
#include "core/db.hpp"
#include "core/fs.hpp"
#include "core/ignore.hpp"

namespace xst {

// Where block data comes from: a request to a device that has the file (net/model wires this to BEP Requests).
// Returns the data, or nullopt if no device could serve it.
using BlockFetcher = std::function<std::optional<bep::Bytes>(const std::vector<DeviceId> &devices,
	const bep::FileInfo &file, const bep::BlockInfo &block, int block_index)>;

struct PullOptions {
	std::string folder_id;
	uint64_t short_id = 0;
	bool ignore_permissions = false;
	int max_conflicts = 10;
	// The temp name for a file: upstream's ".syncthing.<name>.tmp" where it fits, else ".st~<8 hex>.tmp" (always
	// within 42 bytes, for FATX).
	size_t max_name_bytes = 255;
	// fsync each file before renaming it into place (the folder's disableFsync turns it off, as upstream).
	bool fsync = true;
	// Where the time went (tools/soak.cpp): seconds in database calls, in fetching blocks, and in total.
	struct Timing {
		double db = 0, fetch = 0, needed_query = 0, total = 0;
		// Filesystem steps of a file pull.
		double stat = 0, mkdir = 0, create = 0, write = 0, truncate = 0, sync = 0, close = 0, mtime = 0, rename = 0;
	};
	Timing *timing = nullptr;
	const std::atomic<bool> *cancel = nullptr;
	// Wall clock for conflict names (tests pin it).
	std::function<std::string()> conflict_timestamp;
	// Each file pulled into place (for RemoteChangeDetected).
	std::function<void(const bep::FileInfo &)> on_pulled;
	// Called as each file finishes (name, error or "").
	std::function<void(const std::string &, const std::string &)> on_item;
};

struct PullResult {
	int64_t pulled = 0;
	int64_t recorded = 0; // local index entries written (pulled, or recorded as unsupported): announce if > 0
	int64_t bytes_fetched = 0;
	int64_t bytes_reused = 0;
	int64_t conflicts = 0;
	std::map<std::string, std::string> errors; // name -> why, as the folder's pull errors
};

// Brings the folder to the global state for every needed name, then records what it did in the local index.
PullResult pull(fs::Filesystem &fs, Db &db, const IgnoreMatcher &ignores, const BlockFetcher &fetch,
	const PullOptions &options);

std::string temp_name(const std::string &name, size_t max_name_bytes);
// "dir/a.txt" -> "dir/a.sync-conflict-20260124-101500-ABCDEFG.txt"
std::string conflict_name(const std::string &name, const std::string &timestamp, const std::string &short_device);
bool is_conflict_name(const std::string &name);

} // namespace xst
