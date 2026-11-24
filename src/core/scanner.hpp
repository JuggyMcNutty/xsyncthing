#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/bep.hpp"
#include "core/db.hpp"
#include "core/fs.hpp"
#include "core/ignore.hpp"

namespace xst {

// The scanner: walks a folder, and for each file whose size or mtime differs from the local index (or that's new),
// hashes it into blocks and records a new version; names gone from disk become deletions. As lib/scanner and the
// folder's scan in lib/model.
struct ScanOptions {
	std::string folder_id;
	uint64_t short_id = 0;            // our device's short ID, for version counters and modified_by
	bool ignore_permissions = false;  // FATX/FAT32/exFAT: always
	int64_t mtime_window_ns = 0;      // mtimes closer than this are equal (FATX: 2 s)
	const std::atomic<bool> *cancel = nullptr;
	// Progress: bytes hashed so far.
	std::function<void(int64_t)> progress;
};

struct ScanResult {
	int64_t changed = 0;
	int64_t deleted = 0;
	int64_t hashed_bytes = 0;
	std::vector<std::string> errors; // per-file problems (unreadable, ...)
};

// Block size for a file of this size: the smallest power of two from 128 KiB to 16 MiB that keeps the file under
// 2000 blocks (lib/protocol BlockSize).
int32_t block_size_for(int64_t file_size);
// The blocks of data at block_size (one empty block for an empty file).
std::vector<bep::BlockInfo> hash_blocks(fs::File &file, int64_t size, int32_t block_size, int64_t *hashed = nullptr,
	const std::atomic<bool> *cancel = nullptr);
// SHA-256 over the blocks' hashes (lib/protocol BlocksHash).
bep::Bytes blocks_hash(const std::vector<bep::BlockInfo> &blocks);

ScanResult scan(fs::Filesystem &fs, Db &db, const IgnoreMatcher &ignores, const ScanOptions &options,
	const std::string &subdir = "");

// Loads .stignore from the folder root into matcher; false (with error) on a bad pattern.
bool load_ignores(fs::Filesystem &fs, IgnoreMatcher &matcher, std::string *error);

} // namespace xst
