#include "core/puller.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

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

namespace {
struct Timer {
	double *acc;
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	explicit Timer(double *a) : acc(a) {}
	~Timer()
	{
		if (acc)
			*acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
};
} // namespace

PullResult pull(fs::Filesystem &fs, Db &db, const IgnoreMatcher &ignores, const BlockFetcher &fetch_in,
	const PullOptions &options)
{
	PullOptions::Timing scratch;
	PullOptions::Timing &tm = options.timing ? *options.timing : scratch;
	Timer total_timer(&tm.total);
	const BlockFetcher fetch = [&](const std::vector<DeviceId> &d, const bep::FileInfo &f, const bep::BlockInfo &b, int i) {
		Timer t(&tm.fetch);
		return fetch_in(d, f, b, i);
	};
	PullResult result;
	const DeviceId local = Db::local_device();
	const auto stamp = options.conflict_timestamp ? options.conflict_timestamp : default_timestamp;


	auto record = [&](bep::FileInfo f) {
		Timer t(&tm.db);
		f.sequence = db.next_local_sequence(options.folder_id);
		f.local_flags = 0;
		db.update(options.folder_id, local, { f });
		result.recorded++;
	};
	auto record_unsupported = [&](const bep::FileInfo &g) {
		Timer t(&tm.db);
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
		if (options.on_pulled)
			options.on_pulled(f);
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

	std::unordered_set<std::string> known_dirs;
	auto pull_one = [&](const bep::FileInfo &g) {
		if (ignores.match(g.name).ignored)
			return;
		if (const auto why = fs::unstorable_reason(g.name, g.size, fs.rules()); !why.empty()) {
			// Record it as unsupported at the global version (announced as invalid, as upstream does for kinds it
			// can't store), so it isn't needed forever; it stays listed as a folder error.
			if (!g.deleted)
				record_unsupported(g);
			else
				record(g);
			fail(g, why);
			return;
		}
		const auto cur = [&] {
			Timer t(&tm.db);
			return db.get(options.folder_id, local, g.name);
		}();
		const bool have_cur = cur && !cur->deleted && !is_invalid(*cur);
		// A local change concurrent with the global one, or unscanned local edits: keep ours as a conflict copy
		// unless ours wins (then the global version is ours after the next index exchange).
		// Content already equal to the global version (blocks hash and size match) is never a conflict, whatever
		// the versions say: lib/protocol InConflictWith and the puller's shortcut treat it the same way.
		const bool same_content = have_cur && cur->type == bep::FileInfoType::file &&
			g.type == bep::FileInfoType::file && !g.blocks_hash.empty() && cur->blocks_hash == g.blocks_hash &&
			cur->size == g.size && !changed_on_disk(*cur);
		const bool conflict =
			have_cur && !same_content && (vec::concurrent(cur->version, g.version) || changed_on_disk(*cur));

		if (g.deleted) {
			if (have_cur && changed_on_disk(*cur)) {
				fail(g, "the file was modified locally; not deleting");
				return;
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
				known_dirs.erase(g.name);
			}
			if (!ok) {
				fail(g, "deleting: " + fs.last_error());
				return;
			}
			record(g);
			done(g);
			return;
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
				return;
			}
			if (!options.ignore_permissions && !g.no_permissions)
				fs.chmod(g.name, g.permissions);
			record(g);
			done(g);
			return;
		}

		if (g.type != bep::FileInfoType::file) {
			// Symlinks: not supported on the console's volumes. Record as unsupported so we don't need it forever.
			record_unsupported(g);
			fail(g, "symlinks are not supported");
			return;
		}

		// Metadata-only change: same blocks as ours, so just update the mtime/permissions.
		if (have_cur && !conflict && cur->type == bep::FileInfoType::file && cur->blocks_hash == g.blocks_hash &&
			!g.blocks_hash.empty() && cur->size == g.size) {
			fs.set_mtime(g.name, g.modified_s, g.modified_ns);
			if (!options.ignore_permissions && !g.no_permissions)
				fs.chmod(g.name, g.permissions);
			record(g);
			done(g);
			return;
		}

		// A file already on disk that the index doesn't know (a pull cut short by a crash or power loss renamed it
		// into place, but its record wasn't committed): if it already is the global version, adopt it rather than
		// fetch it again and keep the old one as a conflict copy.
		if (!have_cur && g.type == bep::FileInfoType::file && !g.blocks_hash.empty()) {
			const auto st = [&] {
				Timer t(&tm.stat);
				return fs.stat(g.name);
			}();
			if (st && st->kind == fs::Kind::file && st->size == g.size) {
				if (auto in = fs.open_read(g.name)) {
					const auto blocks = hash_blocks(*in, g.size, g.block_size > 0 ? g.block_size : block_size_for(g.size));
					in->close();
					if (blocks_hash(blocks) == g.blocks_hash) {
						fs.set_mtime(g.name, g.modified_s, g.modified_ns);
						record(g);
						done(g);
						return;
					}
				}
			}
		}

		const std::string parent(fs::parent(g.name));
		bool parent_ok = true;
		if (!parent.empty() && !known_dirs.count(parent)) {
			// Each path lookup on FATX scans the directory linearly: remember the directories we know exist rather
			// than asking the volume again for every file (tools/soak.cpp measured 8.5 ms a file on the console).
			Timer t(&tm.mkdir);
			parent_ok = fs.mkdir_all(parent);
			if (parent_ok)
				for (std::string d = parent; !d.empty(); d = std::string(fs::parent(d)))
					known_dirs.insert(d);
		}
		if (!parent_ok) {
			fail(g, "creating parent directory: " + fs.last_error());
			return;
		}
		const std::string tmp = temp_name(g.name, options.max_name_bytes);
		auto out = [&] {
			Timer t(&tm.create);
			return fs.create(tmp);
		}();
		if (!out) {
			fail(g, "creating temp file: " + fs.last_error());
			return;
		}
		const auto devices = [&] {
			Timer t(&tm.db);
			return db.availability(options.folder_id, g.name);
		}();
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
			const auto hit = [&]() -> std::optional<Db::BlockRef> {
				if (data)
					return std::nullopt;
				Timer t(&tm.db);
				return db.find_local_block(options.folder_id, b.hash);
			}();
			if (hit) {
				if (auto src = fs.open_read(hit->name)) {
					bep::Bytes buf(static_cast<size_t>(hit->size));
					if (src->read_at(buf, hit->offset) == hit->size) {
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
			long wrote;
			{
				Timer t(&tm.write);
				wrote = out->write_at(*data, b.offset);
			}
			if (wrote != static_cast<long>(data->size()))
				error = "writing: " + fs.last_error();
			else
				written.emplace(key, std::make_pair(b.offset, b.size));
		}
		bool truncated = true;
		if (error.empty()) {
			Timer t(&tm.truncate);
			truncated = out->truncate(g.size);
		}
		if (!truncated)
			error = "truncating: " + fs.last_error();
		if (error.empty() && options.fsync) {
			Timer t(&tm.sync);
			out->sync();
		}
		{
			Timer t(&tm.close);
			out->close();
		}
		out.reset();
		if (!error.empty()) {
			fs.remove(tmp);
			fail(g, error);
			return;
		}
		if (!options.ignore_permissions && !g.no_permissions)
			fs.chmod(tmp, g.permissions);
		{
			Timer t(&tm.mtime);
			fs.set_mtime(tmp, g.modified_s, g.modified_ns);
		}

		// Something unexpected in the way: keep it as a conflict copy.
		const auto in_the_way = [&] {
			Timer t(&tm.stat);
			return fs.stat(g.name);
		}();
		if (auto st = in_the_way) {
			if (conflict) {
				move_for_conflict(g.name, cur->modified_by);
			} else if (!cur || changed_on_disk(*cur)) {
				move_for_conflict(g.name, cur ? cur->modified_by : options.short_id);
			} else if (st->kind == fs::Kind::directory) {
				fs.remove(g.name);
			}
		}
		bool renamed;
		{
			Timer t(&tm.rename);
			renamed = fs.rename(tmp, g.name);
		}
		if (!renamed) {
			fs.remove(tmp);
			fail(g, "renaming into place: " + fs.last_error());
			return;
		}
		// The volume may have rounded the mtime (FATX: 2 s): record the file as given; the scanner's mtime window
		// treats them as equal.
		record(g);
		done(g);
		};

	// Commit as it goes: a long pull's progress survives a power cut, and readers see it.
	size_t since_commit = 0;
	db.begin();
	// The time each_needed spends outside the callback is its own page queries.
	auto last = std::chrono::steady_clock::now();
	auto last_commit = last;
	db.each_needed(options.folder_id, [&](const bep::FileInfo &g) {
		tm.needed_query += std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count();
		// Each commit flushes the journal to disk: about 35 ms on the console's hard disk (tools/soak.cpp), so commit
		// every 1000 files, or every 5 s, whichever comes first.
		if (++since_commit >= 1000 || std::chrono::steady_clock::now() - last_commit > std::chrono::seconds(5)) {
			Timer t(&tm.db);
			db.commit();
			db.begin();
			since_commit = 0;
			last_commit = std::chrono::steady_clock::now();
		}
		pull_one(g);
		last = std::chrono::steady_clock::now();
		return !(options.cancel && options.cancel->load());
	});
	{
		Timer t(&tm.db);
		db.commit();
	}
	return result;
}

} // namespace xst
