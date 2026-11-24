// The posix backend: used by the host build, and on the console for paths xsdk maps to POSIX calls (the xsdk
// backend wraps it with the volume's rules).
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/fs.hpp"

namespace xst::fs {

namespace {

class PosixFile : public File {
public:
	explicit PosixFile(int fd) : fd_(fd) {}
	~PosixFile() override { close(); }
	long read_at(std::span<uint8_t> buffer, int64_t offset) override
	{
		size_t done = 0;
		while (done < buffer.size()) {
			const ssize_t n = ::pread(fd_, buffer.data() + done, buffer.size() - done, offset + static_cast<off_t>(done));
			if (n < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (n == 0)
				break;
			done += static_cast<size_t>(n);
		}
		return static_cast<long>(done);
	}
	long write_at(std::span<const uint8_t> data, int64_t offset) override
	{
		size_t done = 0;
		while (done < data.size()) {
			const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, offset + static_cast<off_t>(done));
			if (n < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			done += static_cast<size_t>(n);
		}
		return static_cast<long>(done);
	}
	bool truncate(int64_t size) override { return ::ftruncate(fd_, static_cast<off_t>(size)) == 0; }
	bool sync() override { return ::fsync(fd_) == 0; }
	void close() override
	{
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

private:
	int fd_;
};

Stat to_stat(const struct stat &st)
{
	Stat s;
	if (S_ISDIR(st.st_mode))
		s.kind = Kind::directory;
	else if (S_ISREG(st.st_mode))
		s.kind = Kind::file;
#ifdef S_ISLNK
	else if (S_ISLNK(st.st_mode))
		s.kind = Kind::symlink;
#endif
	else
		s.kind = Kind::other;
	s.size = static_cast<int64_t>(st.st_size);
#if defined(__XSDK__)
	s.mtime_s = static_cast<int64_t>(st.st_mtime);
	s.mtime_ns = 0;
#else
	s.mtime_s = static_cast<int64_t>(st.st_mtim.tv_sec);
	s.mtime_ns = static_cast<int32_t>(st.st_mtim.tv_nsec);
#endif
	s.mode = static_cast<uint32_t>(st.st_mode & 07777);
	return s;
}

class PosixFs : public Filesystem {
public:
	PosixFs(std::string root, VolumeRules rules) : root_(std::move(root)), rules_(std::move(rules)) {}

	const std::string &root() const override { return root_; }
	const VolumeRules &rules() const override { return rules_; }

	std::optional<Stat> stat(std::string_view path) override
	{
		struct stat st;
#if defined(__XSDK__)
		if (::stat(full(path).c_str(), &st) != 0)
			return fail<Stat>();
#else
		if (::lstat(full(path).c_str(), &st) != 0)
			return fail<Stat>();
#endif
		return to_stat(st);
	}

	std::optional<std::vector<DirEntry>> list(std::string_view dir) override
	{
		DIR *d = ::opendir(full(dir).c_str());
		if (!d)
			return fail<std::vector<DirEntry>>();
		std::vector<DirEntry> out;
		while (dirent *e = ::readdir(d)) {
			const std::string name = e->d_name;
			if (name == "." || name == "..")
				continue;
			const std::string rel = join(dir, name);
			if (auto st = stat(rel))
				out.push_back({ name, *st });
		}
		::closedir(d);
		return out;
	}

	std::unique_ptr<File> open_read(std::string_view path) override
	{
		const int fd = ::open(full(path).c_str(), O_RDONLY);
		if (fd < 0)
			return fail_ptr();
		return std::make_unique<PosixFile>(fd);
	}

	std::unique_ptr<File> create(std::string_view path) override
	{
		const int fd = ::open(full(path).c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return fail_ptr();
		return std::make_unique<PosixFile>(fd);
	}

	bool mkdir(std::string_view path) override
	{
		if (::mkdir(full(path).c_str(), 0755) == 0 || errno == EEXIST)
			return true;
		return note();
	}

	bool mkdir_all(std::string_view path) override
	{
		if (path.empty())
			return true;
		if (auto st = stat(path))
			return st->kind == Kind::directory;
		if (!mkdir_all(parent(path)))
			return false;
		return mkdir(path);
	}

	bool remove(std::string_view path) override
	{
		const std::string f = full(path);
		if (::unlink(f.c_str()) == 0)
			return true;
		if ((errno == EISDIR || errno == EPERM || errno == EACCES) && ::rmdir(f.c_str()) == 0)
			return true;
		return note();
	}

	bool rename(std::string_view from, std::string_view to) override
	{
		if (::rename(full(from).c_str(), full(to).c_str()) == 0)
			return true;
		// Some volumes (FATX through xsdk) refuse to replace: remove the target and try again.
		if (errno == EEXIST || errno == EACCES || errno == EPERM) {
			::unlink(full(to).c_str());
			if (::rename(full(from).c_str(), full(to).c_str()) == 0)
				return true;
		}
		return note();
	}

	bool set_mtime(std::string_view path, int64_t s, int32_t ns) override
	{
		struct timeval tv[2];
		tv[0].tv_sec = static_cast<time_t>(s);
		tv[0].tv_usec = ns / 1000;
		tv[1] = tv[0];
		if (::utimes(full(path).c_str(), tv) == 0)
			return true;
		return note();
	}

	bool chmod(std::string_view path, uint32_t mode) override
	{
		if (!rules_.keeps_permissions)
			return true;
		if (::chmod(full(path).c_str(), static_cast<mode_t>(mode & 0777)) == 0)
			return true;
		return note();
	}

	std::optional<int64_t> free_space() override
	{
		struct statvfs sv;
		if (::statvfs(root_.c_str(), &sv) != 0)
			return fail<int64_t>();
		return static_cast<int64_t>(sv.f_bavail) * static_cast<int64_t>(sv.f_frsize);
	}

	std::string last_error() const override { return error_; }

private:
	std::string full(std::string_view rel) const { return rel.empty() ? root_ : root_ + "/" + std::string(rel); }
	bool note()
	{
		error_ = std::strerror(errno);
		return false;
	}
	template <class T> std::optional<T> fail()
	{
		note();
		return std::nullopt;
	}
	std::unique_ptr<File> fail_ptr()
	{
		note();
		return nullptr;
	}

	std::string root_;
	VolumeRules rules_;
	std::string error_;
};

} // namespace

std::unique_ptr<Filesystem> open_posix_with_rules(const std::string &root, const VolumeRules &rules)
{
	return std::make_unique<PosixFs>(root, rules);
}

std::unique_ptr<Filesystem> open_posix(const std::string &root) { return open_posix_with_rules(root, VolumeRules{}); }

} // namespace xst::fs
