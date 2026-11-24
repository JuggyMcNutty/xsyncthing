// The console's volumes: which file system a path is on, and the rules that follow (docs/COMPAT.md).
//   FATX (the internal hard disk, memory units): 42-byte names, files under 4 GB, no permissions, 2 s mtimes
//   FAT32 (USB drives as the dashboard formats them): 255-character names, files under 4 GB, 2 s mtimes
//   exFAT (USB or hard disk through xsdk's plugin): 255-character names, no size limit, 10 ms mtimes
#include <cstring>
#include <string>

#include "core/fs.hpp"
#include "platform/platform.hpp"

extern "C" {
#include "xsdk/kernel.h"
}

namespace xst::fs {

std::unique_ptr<Filesystem> open_posix_with_rules(const std::string &root, const VolumeRules &rules);

namespace {

// NtQueryVolumeInformationFile's attribute class (FileFsAttributeInformation = 5, xenia volume.h).
constexpr uint32_t kFsAttributeClass = 5;
struct FsAttributeInformation {
	uint32_t attributes;
	int32_t component_name_max_length;
	uint32_t name_length;
	char name[32];
};

// "hdd:/x/y" -> the kernel's view of the drive root, as xsdk resolves it (src/path.c).
std::string device_root(const std::string &path)
{
	const auto colon = path.find(':');
	const std::string drive = colon == std::string::npos ? "game" : path.substr(0, colon);
	return drive + ":\\";
}

std::string fs_name(const std::string &path, int32_t *max_component)
{
	std::string root = device_root(path);
	ANSI_STRING name{ static_cast<uint16_t>(root.size()), static_cast<uint16_t>(root.size() + 1), root.data() };
	OBJECT_ATTRIBUTES attrs{};
	attrs.root_directory = OB_DOS_DEVICES;
	attrs.name = &name;
	IO_STATUS_BLOCK io{};
	HANDLE h = 0;
	NTSTATUS st = NtCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &attrs, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE);
	if (!NT_SUCCESS(st))
		return "";
	FsAttributeInformation info{};
	st = NtQueryVolumeInformationFile(h, &io, &info, sizeof info, kFsAttributeClass);
	NtClose(h);
	if (!NT_SUCCESS(st))
		return "";
	*max_component = static_cast<int32_t>(info.component_name_max_length);
	const uint32_t n = info.name_length < sizeof info.name ? info.name_length : sizeof info.name;
	return std::string(info.name, n);
}

} // namespace

VolumeRules console_rules(const std::string &path)
{
	int32_t max_component = 0;
	std::string type = fs_name(path, &max_component);
	for (auto &c : type)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	const std::string drive = path.substr(0, path.find(':'));
	if (type.empty())
		type = (drive.starts_with("usb")) ? "FAT32" : "FATX";
	VolumeRules r;
	r.keeps_permissions = false;
	r.supports_symlinks = false;
	r.case_insensitive = true;
	r.forbidden_chars = R"(<>:"\|?*)";
	if (type.find("EXFAT") != std::string::npos) {
		r.type = "exfat";
		r.max_name_bytes = 255;
		r.mtime_window_ns = 10000000LL;
	} else if (type.find("FAT32") != std::string::npos || (type == "FAT" && drive.starts_with("usb"))) {
		r.type = "fat32";
		r.max_name_bytes = 255;
		r.max_file_size = 0xFFFFFFFFLL;
		r.mtime_window_ns = 2000000000LL;
	} else {
		r.type = "fatx";
		r.max_name_bytes = 42;
		r.max_file_size = 0xFFFFFFFFLL;
		r.mtime_window_ns = 2000000000LL;
	}
	if (max_component > 0 && static_cast<size_t>(max_component) < r.max_name_bytes)
		r.max_name_bytes = static_cast<size_t>(max_component);
	platform::log("volume: " + path + " is " + r.type + " (kernel says \"" + type + "\", names up to " +
		std::to_string(r.max_name_bytes) + ")");
	return r;
}

std::unique_ptr<Filesystem> open_console(const std::string &path)
{
	return open_posix_with_rules(path, console_rules(path));
}

} // namespace xst::fs
