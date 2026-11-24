#include "core/puller.hpp"

#include <algorithm>
#include <ctime>
#include <unordered_map>

#include "core/base32.hpp"
#include "core/scanner.hpp"
#include "core/sha256.hpp"
#include "core/vector.hpp"

namespace xst {

namespace {

std::string hex(std::span<const uint8_t> b)
{
	static const char *d = "0123456789abcdef";
	std::string s;
	for (uint8_t c : b) {
		s.push_back(d[c >> 4]);
		s.push_back(d[c & 15]);
	}
	return s;
}

std::string ext_of(const std::string &base)
{
	const auto dot = base.rfind('.');
	if (dot == std::string::npos || dot == 0)
		return "";
	return base.substr(dot);
}

std::string default_timestamp()
{
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
	localtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
	return buf;
}

std::string short_string(uint64_t short_id)
{
	uint8_t b[8];
	for (int i = 0; i < 8; i++)
		b[i] = static_cast<uint8_t>(short_id >> (56 - 8 * i));
	return base32::encode(b).substr(0, 7);
}

// Where to find a block's data locally: in another file we already have.
struct LocalBlock {
	std::string name;
	int64_t offset;
	int32_t size;
};

} // namespace

std::string temp_name(const std::string &name, size_t max_name_bytes)
{
	const std::string dir(fs::parent(name));
	const std::string b(fs::base(name));
	std::string t = ".syncthing." + b + ".tmp";
	if (t.size() > max_name_bytes || t.size() > 160) {
		const auto h = sha256(std::span(reinterpret_cast<const uint8_t *>(b.data()), b.size()));
		t = ".st~" + hex(std::span(h.data(), 4)) + ".tmp";
	}
	return fs::join(dir, t);
}

std::string conflict_name(const std::string &name, const std::string &timestamp, const std::string &short_device)
{
	const std::string ext = ext_of(std::string(fs::base(name)));
	return name.substr(0, name.size() - ext.size()) + ".sync-conflict-" + timestamp + "-" + short_device + ext;
}

bool is_conflict_name(const std::string &name)
{
	return std::string(fs::base(name)).find(".sync-conflict-") != std::string::npos;
}

PullResult pull(fs::Filesystem &fs, Db &db, const IgnoreMatcher &ignores, const BlockFetcher &fetch,
	const PullOptions &options)
{
	PullResult result;
	const DeviceId local = Db::local_device();
	const auto stamp = options.conflict_timestamp ? options.conflict_timestamp : default_timestamp;

	std::vector<bep::FileInfo> needed;
	db.each_needed(options.folder_id, [&](const bep::FileInfo &f) {
		needed.push_back(f);
		return true;
	});
	if (needed.empty())
		return result;

	// Index local blocks by hash so identical blocks are copied, not fetched.
	std::unordered_map<std::string, LocalBlock> local_blocks;
	db.each_local(options.folder_id, [&](const bep::FileInfo &f) {
		if (f.deleted || f.type != bep::FileInfoType::file || is_invalid(f))
			return true;
		for (const auto &b : f.blocks)
			local_blocks.emplace(std::string(b.hash.begin(), b.hash.end()), LocalBlock{ f.name, b.offset, b.size });
		return true;
	});

	auto record = [&](bep::FileInfo f) {
		f.sequence = db.next_local_sequence(options.folder_id);
		f.local_flags = 0;
		db.update(options.folder_id, local, { f });
		result.recorded++;
	};
	auto record_unsupported = [&](const bep::FileInfo &g) {
		bep::FileInfo u = g;
		u.local_flags = kFlagUnsupported;
		u.sequence = db.next_local_sequence(options.folder_id);
		db.update(options.folder_id, local, { u });
		result.recorded++;
	};
	auto fail = [&](const bep::FileInfo &f, const std::string &why) {
		result.errors[f.name] = why;
		if (options.on_item)
			options.on_item(f.name, why);
	};
	auto done = [&](const bep::FileInfo &f) {
		result.pulled++;
		if (options.on_item)
			options.on_item(f.name, "");
	};

	// What's on disk vs what the index says we have: if they differ, the local copy has unscanned changes.
	auto changed_on_disk = [&](const bep::FileInfo &cur) {
		const auto st = fs.stat(cur.name);
		if (!st)
			return !cur.deleted;
		if (cur.deleted)
			return true;
		if (cur.type == bep::FileInfoType::directory)
			return st->kind != fs::Kind::directory;
		const int64_t a = cur.modified_s * 1000000000LL + cur.modified_ns;
		const int64_t b = st->mtime_s * 1000000000LL + st->mtime_ns;
		const int64_t diff = a > b ? a - b : b - a;
		return st->kind != fs::Kind::file || st->size != cur.size ||
			(diff != 0 && diff >= fs.rules().mtime_window_ns);
	};

	auto move_for_conflict = [&](const std::string &name, uint64_t last_modified_by) {
		if (is_conflict_name(name) || options.max_conflicts == 0) {
			fs.remove(name);
			return;
		}
		const std::string cn = conflict_name(name, stamp(), short_string(last_modified_by));
		if (fs.rename(name, cn))
			result.conflicts++;
	};

	db.begin();
	for (const auto &g : needed) {
		if (options.cancel && options.cancel->load())
			break;
		if (ignores.match(g.name).ignored)
			continue;
		if (const auto why = fs::unstorable_reason(g.name, g.size, fs.rules()); !why.empty()) {
			// Record it as unsupported at the global version (announced as invalid, as upstream does for kinds it
			// can't store), so it isn't needed forever; it stays listed as a folder error.
			if (!g.deleted)
				record_unsupported(g);
			else
				record(g);
			fail(g, why);
			continue;
		}
		const auto cur = db.get(options.folder_id, local, g.name);
		const bool have_cur = cur && !cur->deleted && !is_invalid(*cur);
		// A local change concurrent with the global one, or unscanned local edits: keep ours as a conflict copy
		// unless ours wins (then the global version is ours after the next index exchange).
		const bool conflict = have_cur && (vec::concurrent(cur->version, g.version) || changed_on_disk(*cur));

		if (g.deleted) {
			if (have_cur && changed_on_disk(*cur)) {
				fail(g, "the file was modified locally; not deleting");
				continue;
			}
			bool ok = true;
			if (auto st = fs.stat(g.name)) {
				if (st->kind == fs::Kind::directory) {
					// Remove ignored-deletable leftovers, then the directory if empty.
					if (auto entries = fs.list(g.name)) {
						for (const auto &e : *entries) {
							const std::string child = fs::join(g.name, e.name);
							if (ignores.match(child).deletable || IgnoreMatcher::is_temporary(child))
								fs.remove(child);
						}
					}
				}
				ok = fs.remove(g.name);
			}
			if (!ok) {
				fail(g, "deleting: " + fs.last_error());
				continue;
			}
			record(g);
			done(g);
			continue;
		}

		if (g.type == bep::FileInfoType::directory) {
			if (auto st = fs.stat(g.name); st && st->kind != fs::Kind::directory) {
				if (conflict || (cur && changed_on_disk(*cur)))
					move_for_conflict(g.name, cur ? cur->modified_by : 0);
				else
					fs.remove(g.name);
			}
			if (!fs.mkdir_all(g.name)) {
				fail(g, "creating directory: " + fs.last_error());
				continue;
			}
			if (!options.ignore_permissions && !g.no_permissions)
				fs.chmod(g.name, g.permissions);
			record(g);
			done(g);
			continue;
		}

		if (g.type != bep::FileInfoType::file) {
			// Symlinks: not supported on the console's volumes. Record as unsupported so we don't need it forever.
			record_unsupported(g);
			fail(g, "symlinks are not supported");
			continue;
		}

		// Metadata-only change: same blocks as ours, so just update the mtime/permissions.
		if (have_cur && !conflict && cur->type == bep::FileInfoType::file && cur->blocks_hash == g.blocks_hash &&
			!g.blocks_hash.empty() && cur->size == g.size) {
			fs.set_mtime(g.name, g.modified_s, g.modified_ns);
			if (!options.ignore_permissions && !g.no_permissions)
				fs.chmod(g.name, g.permissions);
			record(g);
			done(g);
			continue;
		}

		const std::string parent(fs::parent(g.name));
		if (!parent.empty() && !fs.mkdir_all(parent)) {
			fail(g, "creating parent directory: " + fs.last_error());
			continue;
		}
		const std::string tmp = temp_name(g.name, options.max_name_bytes);
		auto out = fs.create(tmp);
		if (!out) {
			fail(g, "creating temp file: " + fs.last_error());
			continue;
		}
		const auto devices = db.availability(options.folder_id, g.name);
		std::string error;
		std::unordered_map<std::string, std::pair<int64_t, int32_t>> written; // hash -> offset, size in tmp
		for (size_t i = 0; i < g.blocks.size() && error.empty(); i++) {
			if (options.cancel && options.cancel->load()) {
				error = "cancelled";
				break;
			}
			const auto &b = g.blocks[i];
			if (b.size == 0)
				continue;
			std::optional<bep::Bytes> data;
			const std::string key(b.hash.begin(), b.hash.end());
			if (const auto w = written.find(key); w != written.end()) {
				bep::Bytes buf(static_cast<size_t>(w->second.second));
				if (out->read_at(buf, w->second.first) == w->second.second) {
					data = std::move(buf);
					result.bytes_reused += b.size;
				}
			}
			const auto hit = data ? local_blocks.end() : local_blocks.find(key);
			if (hit != local_blocks.end()) {
				if (auto src = fs.open_read(hit->second.name)) {
					bep::Bytes buf(static_cast<size_t>(hit->second.size));
					if (src->read_at(buf, hit->second.offset) == hit->second.size) {
						const auto h = sha256(buf);
						if (std::equal(h.begin(), h.end(), b.hash.begin(), b.hash.end()))
							data = std::move(buf);
					}
				}
				if (data)
					result.bytes_reused += b.size;
			}
			if (!data) {
				if (devices.empty()) {
					error = "no connected device has the file";
					break;
				}
				data = fetch(devices, g, b, static_cast<int>(i));
				if (!data) {
					error = "fetching block " + std::to_string(i) + " failed";
					break;
				}
				const auto h = sha256(*data);
				if (data->size() != static_cast<size_t>(b.size) || !std::equal(h.begin(), h.end(), b.hash.begin(), b.hash.end())) {
					error = "block " + std::to_string(i) + " has the wrong hash";
					break;
				}
				result.bytes_fetched += b.size;
			}
			if (out->write_at(*data, b.offset) != static_cast<long>(data->size()))
				error = "writing: " + fs.last_error();
			else
				written.emplace(key, std::make_pair(b.offset, b.size));
		}
		if (error.empty() && !out->truncate(g.size))
			error = "truncating: " + fs.last_error();
		if (error.empty())
			out->sync();
		out->close();
		out.reset();
		if (!error.empty()) {
			fs.remove(tmp);
			fail(g, error);
			continue;
		}
		if (!options.ignore_permissions && !g.no_permissions)
			fs.chmod(tmp, g.permissions);
		fs.set_mtime(tmp, g.modified_s, g.modified_ns);

		// Something unexpected in the way: keep it as a conflict copy.
		if (auto st = fs.stat(g.name)) {
			if (conflict) {
				move_for_conflict(g.name, cur->modified_by);
			} else if (!cur || changed_on_disk(*cur)) {
				move_for_conflict(g.name, cur ? cur->modified_by : options.short_id);
			} else if (st->kind == fs::Kind::directory) {
				fs.remove(g.name);
			}
		}
		if (!fs.rename(tmp, g.name)) {
			fs.remove(tmp);
			fail(g, "renaming into place: " + fs.last_error());
			continue;
		}
		// The volume may have rounded the mtime (FATX: 2 s): record the file as given; the scanner's mtime window
		// treats them as equal.
		record(g);
		done(g);
		for (const auto &b : g.blocks)
			local_blocks.emplace(std::string(b.hash.begin(), b.hash.end()), LocalBlock{ g.name, b.offset, b.size });
	}
	db.commit();
	return result;
}

} // namespace xst
