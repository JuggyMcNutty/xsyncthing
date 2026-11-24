#pragma once

// BEP v1 messages (proto/bep.proto) as C++ types, with their protobuf encoding. Hand-written on core/protowire, so
// the console needs no protobuf runtime; field numbers follow bep.proto exactly. Unknown fields are skipped on decode,
// as protobuf does; proto3 defaults are left out on encode, as the reference encoder does.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace xst::bep {

using Bytes = std::vector<uint8_t>;

enum class MessageType : int32_t {
	cluster_config = 0,
	index = 1,
	index_update = 2,
	request = 3,
	response = 4,
	download_progress = 5,
	ping = 6,
	close = 7,
};

enum class MessageCompression : int32_t { none = 0, lz4 = 1 };

struct Header {
	MessageType type = MessageType::cluster_config;
	MessageCompression compression = MessageCompression::none;
};

enum class Compression : int32_t { metadata = 0, never = 1, always = 2 };
enum class FolderType : int32_t { send_receive = 0, send_only = 1, receive_only = 2, receive_encrypted = 3 };
enum class FolderStopReason : int32_t { running = 0, paused = 1 };

struct Device {
	Bytes id; // 32 bytes
	std::string name;
	std::vector<std::string> addresses;
	Compression compression = Compression::metadata;
	std::string cert_name;
	int64_t max_sequence = 0;
	bool introducer = false;
	uint64_t index_id = 0;
	bool skip_introduction_removals = false;
	Bytes encryption_password_token;
};

struct Folder {
	std::string id;
	std::string label;
	FolderType type = FolderType::send_receive;
	FolderStopReason stop_reason = FolderStopReason::running;
	std::vector<Device> devices;
};

struct ClusterConfig {
	std::vector<Folder> folders;
	bool secondary = false;
};

struct Counter {
	uint64_t id = 0;
	uint64_t value = 0;
	friend bool operator==(const Counter &, const Counter &) = default;
};

struct Vector {
	std::vector<Counter> counters;
	friend bool operator==(const Vector &, const Vector &) = default;
};

enum class FileInfoType : int32_t { file = 0, directory = 1, symlink_file = 2, symlink_directory = 3, symlink = 4 };

struct BlockInfo {
	int64_t offset = 0;
	int32_t size = 0;
	Bytes hash; // 32 bytes (48 for encrypted folders)
	friend bool operator==(const BlockInfo &, const BlockInfo &) = default;
};

// PlatformData is kept as its raw encoding: xsyncthing doesn't apply ownership or xattrs (FATX has neither) but
// passes them on unchanged.
struct FileInfo {
	std::string name;
	FileInfoType type = FileInfoType::file;
	int64_t size = 0;
	uint32_t permissions = 0;
	int64_t modified_s = 0;
	int32_t modified_ns = 0;
	uint64_t modified_by = 0;
	bool deleted = false;
	bool invalid = false;
	bool no_permissions = false;
	Vector version;
	int64_t sequence = 0;
	int32_t block_size = 0;
	std::vector<BlockInfo> blocks;
	Bytes symlink_target;
	Bytes blocks_hash;
	Bytes previous_blocks_hash;
	Bytes encrypted;
	Bytes platform; // raw PlatformData
	// Local only (not on the wire): see bep.proto.
	uint32_t local_flags = 0;
};

struct Index {
	std::string folder;
	std::vector<FileInfo> files;
	int64_t last_sequence = 0;
};

struct IndexUpdate {
	std::string folder;
	std::vector<FileInfo> files;
	int64_t last_sequence = 0;
	int64_t prev_sequence = 0;
};

struct Request {
	int32_t id = 0;
	std::string folder;
	std::string name;
	int64_t offset = 0;
	int32_t size = 0;
	Bytes hash;
	bool from_temporary = false;
	int32_t block_no = 0;
};

enum class ErrorCode : int32_t { no_error = 0, generic = 1, no_such_file = 2, invalid_file = 3 };

struct Response {
	int32_t id = 0;
	Bytes data;
	ErrorCode code = ErrorCode::no_error;
};

enum class FileDownloadProgressUpdateType : int32_t { append = 0, forget = 1 };

struct FileDownloadProgressUpdate {
	FileDownloadProgressUpdateType update_type = FileDownloadProgressUpdateType::append;
	std::string name;
	Vector version;
	std::vector<int32_t> block_indexes;
	int32_t block_size = 0;
};

struct DownloadProgress {
	std::string folder;
	std::vector<FileDownloadProgressUpdate> updates;
};

struct Ping {};

struct Close {
	std::string reason;
};

using Message = std::variant<ClusterConfig, Index, IndexUpdate, Request, Response, DownloadProgress, Ping, Close>;

MessageType type_of(const Message &message);

Bytes encode(const Header &h);
Bytes encode(const Message &message);
Bytes encode(const FileInfo &f); // also the database's record format
Bytes encode(const Vector &v);

std::optional<Header> decode_header(std::span<const uint8_t> data);
std::optional<Message> decode(MessageType type, std::span<const uint8_t> data);
std::optional<FileInfo> decode_file_info(std::span<const uint8_t> data);
std::optional<Vector> decode_vector(std::span<const uint8_t> data);

// The largest message Syncthing accepts (lib/protocol MaxMessageLen): 500 MB.
inline constexpr uint32_t kMaxMessageLen = 500 * 1000 * 1000;
// Messages at least this big are LZ4-compressed when compression is "always" or for index messages.
inline constexpr size_t kCompressionThreshold = 128;

} // namespace xst::bep
