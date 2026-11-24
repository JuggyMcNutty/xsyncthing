#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The folder's filesystem as the core sees it: paths are relative to the folder root with '/' separators, as BEP
// names are. Backends: posix (host) and xsdk (console, FATX/FAT32/exFAT); both implemented in src/platform.
namespace xst::fs {

enum class Kind { file, directory, symlink, other };

struct Stat {
	Kind kind = Kind::file;
	int64_t size = 0;
	int64_t mtime_s = 0;
	int32_t mtime_ns = 0;
	uint32_t mode = 0; // permission bits; 0 where the volume keeps none
};

struct DirEntry {
	std::string name;
	Stat stat;
};

// What the volume can store; drives the name and size checks (docs/COMPAT.md).
struct VolumeRules {
	std::string type = "posix";   // "posix", "fatx", "fat32", "exfat"
	size_t max_name_bytes = 255;  // one path component
	int64_t max_file_size = INT64_MAX;
	std::string forbidden_chars;  // in a name, besides '/' and NUL
	bool case_insensitive = false;
	bool keeps_permissions = true;
	bool supports_symlinks = true;
	int64_t mtime_window_ns = 0;  // the volume rounds mtimes to this (FATX: 2 s)
};

class File {
public:
	virtual ~File() = default;
	virtual long read_at(std::span<uint8_t> buffer, int64_t offset) = 0;
	virtual long write_at(std::span<const uint8_t> data, int64_t offset) = 0;
	virtual bool truncate(int64_t size) = 0;
	virtual bool sync() = 0;
	virtual void close() = 0;
};

class Filesystem {
public:
	virtual ~Filesystem() = default;
	virtual const std::string &root() const = 0;
	virtual const VolumeRules &rules() const = 0;

	virtual std::optional<Stat> stat(std::string_view path) = 0;
	virtual std::optional<std::vector<DirEntry>> list(std::string_view dir) = 0;
	virtual std::unique_ptr<File> open_read(std::string_view path) = 0;
	// Creates or truncates.
	virtual std::unique_ptr<File> create(std::string_view path) = 0;
	virtual bool mkdir(std::string_view path) = 0;
	virtual bool mkdir_all(std::string_view path) = 0;
	virtual bool remove(std::string_view path) = 0; // a file or an empty directory
	// Replaces to if it exists. Both must be closed first (the console can't rename open files).
	virtual bool rename(std::string_view from, std::string_view to) = 0;
	virtual bool set_mtime(std::string_view path, int64_t s, int32_t ns) = 0;
	virtual bool chmod(std::string_view path, uint32_t mode) = 0;
	virtual std::optional<int64_t> free_space() = 0;
	virtual std::string last_error() const = 0;
};

// The host's backend, rooted at an absolute directory.
std::unique_ptr<Filesystem> open_posix(const std::string &root);

// Why a name from a peer can't be stored on this volume, or "" if it can.
std::string unstorable_reason(std::string_view path, int64_t size, const VolumeRules &rules);

// "dir/sub/file" -> "dir/sub"; "file" -> "".
std::string_view parent(std::string_view path);
std::string_view base(std::string_view path);
std::string join(std::string_view a, std::string_view b);

} // namespace xst::fs
