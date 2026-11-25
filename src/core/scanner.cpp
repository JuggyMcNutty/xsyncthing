#include "core/scanner.hpp"

#include <ctime>
#include <set>

#include "core/sha256.hpp"
#include "core/vector.hpp"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

namespace xst {

namespace {

constexpr int32_t kMinBlock = 128 << 10;
constexpr int32_t kMaxBlock = 16 << 20;
constexpr int64_t kDesiredBlocks = 2000;

bool mtime_equal(int64_t a_s, int32_t a_ns, int64_t b_s, int32_t b_ns, int64_t window_ns)
{
	const int64_t a = a_s * 1000000000LL + a_ns;
	const int64_t b = b_s * 1000000000LL + b_ns;
	const int64_t diff = a > b ? a - b : b - a;
	return diff == 0 || diff < window_ns;
}


} // namespace

int32_t block_size_for(int64_t file_size)
{
	int32_t bs = kMinBlock;
	for (; bs <= kMaxBlock; bs *= 2)
		if (file_size < kDesiredBlocks * bs)
			return bs;
	return kMaxBlock;
}

std::vector<bep::BlockInfo> hash_blocks(fs::File &file, int64_t size, int32_t block_size, int64_t *hashed,
	const std::atomic<bool> *cancel)
{
	std::vector<bep::BlockInfo> blocks;
	std::vector<uint8_t> buf(static_cast<size_t>(block_size));
	int64_t offset = 0;
	while (offset < size) {
		if (cancel && cancel->load())
			return {};
		const auto want = static_cast<size_t>(std::min<int64_t>(block_size, size - offset));
		const long n = file.read_at(std::span(buf.data(), want), offset);
		if (n <= 0)
			break; // shorter than stat said: the file changed under us; the next scan catches it
		const auto digest = sha256(std::span(buf.data(), static_cast<size_t>(n)));
		blocks.push_back({ offset, static_cast<int32_t>(n), bep::Bytes(digest.begin(), digest.end()) });
		offset += n;
		if (hashed)
			*hashed += n;
	}
	if (blocks.empty()) {
		const auto digest = sha256({});
		blocks.push_back({ 0, 0, bep::Bytes(digest.begin(), digest.end()) });
	}
	return blocks;
}

bep::Bytes blocks_hash(const std::vector<bep::BlockInfo> &blocks)
{
	wc_Sha256 h;
	wc_InitSha256(&h);
	for (const auto &b : blocks)
		wc_Sha256Update(&h, b.hash.data(), static_cast<word32>(b.hash.size()));
	bep::Bytes out(32);
	wc_Sha256Final(&h, out.data());
	wc_Sha256Free(&h);
	return out;
}

bool load_ignores(fs::Filesystem &fs, IgnoreMatcher &matcher, std::string *error)
{
	auto read = [&](const std::string &path) -> std::optional<std::string> {
		auto st = fs.stat(path);
		if (!st || st->kind != fs::Kind::file || st->size > (1 << 20))
			return std::nullopt;
		auto f = fs.open_read(path);
		if (!f)
			return std::nullopt;
		std::string text(static_cast<size_t>(st->size), '\0');
		const long n = f->read_at(std::span(reinterpret_cast<uint8_t *>(text.data()), text.size()), 0);
		if (n < 0)
			return std::nullopt;
		text.resize(static_cast<size_t>(n));
		return text;
	};
	const auto text = read(".stignore");
	if (!text)
		return matcher.parse("", nullptr, error);
	return matcher.parse(*text, read, error);
}

ScanResult scan(fs::Filesystem &fs, Db &db, const IgnoreMatcher &ignores, const ScanOptions &options,
	const std::string &subdir)
{
	ScanResult result;
	const DeviceId local = Db::local_device();
	std::set<std::string> seen;
	std::vector<bep::FileInfo> batch;
	auto flush = [&] {
		if (batch.empty())
			return;
		db.begin();
		for (auto &f : batch)
			f.sequence = db.next_local_sequence(options.folder_id);
		db.update(options.folder_id, local, batch);
		db.commit();
		if (options.on_changes)
			options.on_changes(batch);
		batch.clear();
	};
	auto record = [&](bep::FileInfo f, const std::optional<bep::FileInfo> &old) {
		f.version = vec::update(old ? old->version : bep::Vector{}, options.short_id);
		f.modified_by = options.short_id;
		if (old && !old->blocks_hash.empty() && !f.deleted)
			f.previous_blocks_hash = old->blocks_hash;
		batch.push_back(std::move(f));
		result.changed++;
		if (batch.size() >= 500)
			flush();
	};

	// Depth-first walk.
	std::vector<std::string> stack = { subdir };
	while (!stack.empty()) {
		if (options.cancel && options.cancel->load())
			return result;
		const std::string dir = stack.back();
		stack.pop_back();
		auto entries = fs.list(dir);
		if (!entries) {
			result.errors.push_back(dir + ": " + fs.last_error());
			continue;
		}
		for (const auto &e : *entries) {
			const std::string path = fs::join(dir, e.name);
			if (IgnoreMatcher::is_temporary(path) && e.stat.kind == fs::Kind::file) {
				// A pull interrupted by a crash or power cut leaves its temp file; clean those up.
				const int64_t age = static_cast<int64_t>(std::time(nullptr)) - e.stat.mtime_s;
				if (options.temp_lifetime_s == 0 || age > options.temp_lifetime_s) {
					if (fs.remove(path) && options.temps_removed)
						++*options.temps_removed;
				}
				continue;
			}
			const auto ig = ignores.match(path);
			if (ig.ignored) {
				// Ignored names stay in the index as they were, flagged; children of a skippable dir are not walked.
				if (auto old = db.get(options.folder_id, local, path); old && !(old->local_flags & kFlagIgnored)) {
					bep::FileInfo f = *old;
					f.local_flags |= kFlagIgnored;
					f.invalid = false;
					batch.push_back(f);
					flush();
				}
				seen.insert(path);
				if (e.stat.kind == fs::Kind::directory && !ig.can_skip_dir)
					stack.push_back(path);
				continue;
			}
			seen.insert(path);
			if (options.on_file)
				options.on_file();
			const auto old = db.get(options.folder_id, local, path);
			const bool was_valid = old && !old->deleted && !(old->local_flags & kLocalInvalidFlags);

			if (e.stat.kind == fs::Kind::directory) {
				stack.push_back(path);
				if (was_valid && old->type == bep::FileInfoType::directory &&
					(options.ignore_permissions || old->no_permissions ||
						(old->permissions & 0777) == (e.stat.mode & 0777)))
					continue;
				bep::FileInfo f;
				f.name = path;
				f.type = bep::FileInfoType::directory;
				f.permissions = options.ignore_permissions ? 0777 : e.stat.mode & 0777;
				f.no_permissions = options.ignore_permissions;
				f.modified_s = e.stat.mtime_s;
				f.modified_ns = e.stat.mtime_ns;
				record(std::move(f), old);
				continue;
			}
			if (e.stat.kind != fs::Kind::file)
				continue; // symlinks and devices aren't synced by xsyncthing

			if (was_valid && old->type == bep::FileInfoType::file && old->size == e.stat.size &&
				mtime_equal(old->modified_s, old->modified_ns, e.stat.mtime_s, e.stat.mtime_ns,
					options.mtime_window_ns) &&
				(options.ignore_permissions || old->no_permissions ||
					(old->permissions & 0777) == (e.stat.mode & 0777)))
				continue; // unchanged

			auto file = fs.open_read(path);
			if (!file) {
				result.errors.push_back(path + ": " + fs.last_error());
				continue;
			}
			bep::FileInfo f;
			f.name = path;
			f.type = bep::FileInfoType::file;
			f.size = e.stat.size;
			f.permissions = options.ignore_permissions ? 0666 : e.stat.mode & 0777;
			f.no_permissions = options.ignore_permissions;
			f.modified_s = e.stat.mtime_s;
			f.modified_ns = e.stat.mtime_ns;
			f.block_size = block_size_for(f.size);
			f.blocks = hash_blocks(*file, f.size, f.block_size, &result.hashed_bytes, options.cancel);
			file->close();
			if (options.cancel && options.cancel->load())
				return result;
			f.blocks_hash = blocks_hash(f.blocks);
			if (options.progress)
				options.progress(result.hashed_bytes);
			// Same content as before (only the mtime moved, say): keep the version, just refresh the metadata.
			if (was_valid && old->type == bep::FileInfoType::file && old->blocks_hash == f.blocks_hash &&
				old->size == f.size) {
				bep::FileInfo same = *old;
				same.modified_s = f.modified_s;
				same.modified_ns = f.modified_ns;
				same.permissions = f.permissions;
				if (mtime_equal(old->modified_s, old->modified_ns, f.modified_s, f.modified_ns,
						options.mtime_window_ns))
					continue;
				record(std::move(same), old);
				continue;
			}
			record(std::move(f), old);
		}
	}

	// Names in the index under subdir that are gone from disk become deletions.
	std::vector<bep::FileInfo> gone;
	db.each_local(options.folder_id, [&](const bep::FileInfo &f) {
		if (f.deleted || seen.count(f.name))
			return true;
		if (!subdir.empty() && !(f.name == subdir || f.name.starts_with(subdir + "/")))
			return true;
		// Entries we never stored (unsupported: a name the volume can't hold, a symlink) or ignore aren't on disk by
		// design; announcing them as deleted would delete them everywhere else.
		if (f.local_flags & (kFlagIgnored | kFlagUnsupported))
			return true;
		gone.push_back(f);
		return true;
	});
	for (auto &f : gone) {
		bep::FileInfo d;
		d.name = f.name;
		d.type = f.type;
		d.deleted = true;
		d.no_permissions = f.no_permissions;
		d.permissions = f.permissions;
		d.modified_s = f.modified_s;
		d.modified_ns = f.modified_ns;
		record(std::move(d), f);
		result.deleted++;
	}
	flush();
	return result;
}

} // namespace xst
