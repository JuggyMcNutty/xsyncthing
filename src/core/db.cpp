#include "core/db.hpp"

#include <cstring>
#include <random>
#include <stdexcept>

#include <sqlite3.h>

#include "core/vector.hpp"

namespace xst {

namespace {

const char *kSchema = R"SQL(
PRAGMA journal_mode = TRUNCATE;
PRAGMA temp_store = MEMORY;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS files (
	folder TEXT NOT NULL,
	device BLOB NOT NULL,
	name TEXT NOT NULL,
	sequence INTEGER NOT NULL,
	deleted INTEGER NOT NULL,
	invalid INTEGER NOT NULL,
	is_dir INTEGER NOT NULL,
	size INTEGER NOT NULL,
	global INTEGER NOT NULL DEFAULT 0,
	need INTEGER NOT NULL DEFAULT 0,
	fi BLOB NOT NULL,
	PRIMARY KEY (folder, device, name)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS files_seq ON files (folder, device, sequence);
CREATE INDEX IF NOT EXISTS files_name ON files (folder, name);
CREATE INDEX IF NOT EXISTS files_need ON files (folder, need) WHERE need = 1;
CREATE TABLE IF NOT EXISTS index_ids (
	folder TEXT NOT NULL,
	device BLOB NOT NULL,
	index_id INTEGER NOT NULL,
	PRIMARY KEY (folder, device)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS sequences (
	folder TEXT NOT NULL PRIMARY KEY,
	next INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS kv (
	key TEXT NOT NULL PRIMARY KEY,
	value TEXT NOT NULL
) WITHOUT ROWID;
)SQL";

struct Stmt {
	sqlite3_stmt *s = nullptr;
	Stmt(sqlite3 *db, const char *sql)
	{
		if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
			throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) + ": " + sql);
	}
	~Stmt() { sqlite3_finalize(s); }
	Stmt &text(int i, const std::string &v)
	{
		sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
		return *this;
	}
	Stmt &blob(int i, const void *p, size_t n)
	{
		sqlite3_bind_blob(s, i, p, static_cast<int>(n), SQLITE_TRANSIENT);
		return *this;
	}
	Stmt &device(int i, const DeviceId &d) { return blob(i, d.bytes().data(), d.bytes().size()); }
	Stmt &i64(int i, int64_t v)
	{
		sqlite3_bind_int64(s, i, v);
		return *this;
	}
	bool step()
	{
		const int rc = sqlite3_step(s);
		if (rc == SQLITE_ROW)
			return true;
		if (rc != SQLITE_DONE)
			throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
		return false;
	}
	int64_t col_i64(int i) { return sqlite3_column_int64(s, i); }
	std::string col_text(int i)
	{
		const auto *p = sqlite3_column_text(s, i);
		return p ? std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(sqlite3_column_bytes(s, i)))
				 : std::string();
	}
	std::span<const uint8_t> col_blob(int i)
	{
		const auto *p = static_cast<const uint8_t *>(sqlite3_column_blob(s, i));
		return { p, static_cast<size_t>(sqlite3_column_bytes(s, i)) };
	}
	DeviceId col_device(int i)
	{
		const auto b = col_blob(i);
		return DeviceId::from_bytes(b).value_or(DeviceId());
	}
};

bep::FileInfo decode_row(std::span<const uint8_t> blob)
{
	auto f = bep::decode_file_info(blob);
	if (!f)
		throw std::runtime_error("corrupt file record in the database");
	return std::move(*f);
}

int64_t mtime_ns(const bep::FileInfo &f) { return f.modified_s * 1000000000LL + f.modified_ns; }

} // namespace

bool newer_than(const bep::FileInfo &a, const bep::FileInfo &b)
{
	switch (vec::compare(a.version, b.version)) {
	case vec::Ordering::greater:
		return true;
	case vec::Ordering::lesser:
	case vec::Ordering::equal:
		return false;
	default:
		break;
	}
	// Concurrent: lib/protocol WinsConflict.
	if (is_invalid(a) != is_invalid(b))
		return !is_invalid(a);
	if (mtime_ns(a) != mtime_ns(b))
		return mtime_ns(a) > mtime_ns(b);
	return vec::compare(a.version, b.version) == vec::Ordering::concurrent_greater;
}

DeviceId Db::local_device()
{
	DeviceId::Bytes b;
	b.fill(0xFF);
	return DeviceId(b);
}

std::unique_ptr<Db> Db::open(const std::string &path, std::string *error)
{
	sqlite3 *db = nullptr;
	if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
			nullptr) != SQLITE_OK) {
		if (error)
			*error = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
		sqlite3_close(db);
		return nullptr;
	}
	sqlite3_busy_timeout(db, 5000);
	char *msg = nullptr;
	if (sqlite3_exec(db, kSchema, nullptr, nullptr, &msg) != SQLITE_OK) {
		if (error)
			*error = msg ? msg : "schema failed";
		sqlite3_free(msg);
		sqlite3_close(db);
		return nullptr;
	}
	return std::unique_ptr<Db>(new Db(db));
}

Db::~Db() { sqlite3_close(db_); }

void Db::exec(const char *sql)
{
	char *msg = nullptr;
	if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
		std::string e = msg ? msg : "exec failed";
		sqlite3_free(msg);
		throw std::runtime_error("sqlite: " + e);
	}
}

void Db::begin() { exec("BEGIN IMMEDIATE"); }
void Db::commit() { exec("COMMIT"); }

uint64_t Db::index_id(const std::string &folder, const DeviceId &device)
{
	Stmt s(db_, "SELECT index_id FROM index_ids WHERE folder = ?1 AND device = ?2");
	s.text(1, folder).device(2, device);
	return s.step() ? static_cast<uint64_t>(s.col_i64(0)) : 0;
}

void Db::set_index_id(const std::string &folder, const DeviceId &device, uint64_t id)
{
	Stmt s(db_, "INSERT OR REPLACE INTO index_ids (folder, device, index_id) VALUES (?1, ?2, ?3)");
	s.text(1, folder).device(2, device).i64(3, static_cast<int64_t>(id));
	s.step();
}

uint64_t Db::local_index_id(const std::string &folder)
{
	if (const uint64_t id = index_id(folder, local_device()))
		return id;
	std::random_device rd;
	uint64_t id = 0;
	while (id == 0)
		id = (static_cast<uint64_t>(rd()) << 32 | rd()) & 0x7FFFFFFFFFFFFFFFULL;
	set_index_id(folder, local_device(), id);
	return id;
}

int64_t Db::max_sequence(const std::string &folder, const DeviceId &device)
{
	Stmt s(db_, "SELECT COALESCE(MAX(sequence), 0) FROM files WHERE folder = ?1 AND device = ?2");
	s.text(1, folder).device(2, device);
	s.step();
	return s.col_i64(0);
}

int64_t Db::next_local_sequence(const std::string &folder)
{
	int64_t next = 0;
	{
		Stmt s(db_, "SELECT next FROM sequences WHERE folder = ?1");
		s.text(1, folder);
		next = s.step() ? s.col_i64(0) : max_sequence(folder, local_device()) + 1;
	}
	Stmt u(db_, "INSERT OR REPLACE INTO sequences (folder, next) VALUES (?1, ?2)");
	u.text(1, folder).i64(2, next + 1);
	u.step();
	return next;
}

void Db::update(const std::string &folder, const DeviceId &device, const std::vector<bep::FileInfo> &files)
{
	Stmt s(db_, "INSERT OR REPLACE INTO files (folder, device, name, sequence, deleted, invalid, is_dir, size, fi) "
				"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
	for (const auto &f : files) {
		const auto blob = bep::encode(f);
		sqlite3_reset(s.s);
		s.text(1, folder)
			.device(2, device)
			.text(3, f.name)
			.i64(4, f.sequence)
			.i64(5, f.deleted)
			.i64(6, is_invalid(f))
			.i64(7, f.type == bep::FileInfoType::directory)
			.i64(8, f.size)
			.blob(9, blob.data(), blob.size());
		s.step();
		recompute_global(folder, f.name);
	}
}

void Db::drop_device(const std::string &folder, const DeviceId &device)
{
	std::vector<std::string> names;
	{
		Stmt s(db_, "SELECT name FROM files WHERE folder = ?1 AND device = ?2");
		s.text(1, folder).device(2, device);
		while (s.step())
			names.push_back(s.col_text(0));
	}
	Stmt d(db_, "DELETE FROM files WHERE folder = ?1 AND device = ?2");
	d.text(1, folder).device(2, device);
	d.step();
	for (const auto &n : names)
		recompute_global(folder, n);
}

void Db::drop_folder(const std::string &folder)
{
	for (const char *sql : { "DELETE FROM files WHERE folder = ?1", "DELETE FROM index_ids WHERE folder = ?1",
			 "DELETE FROM sequences WHERE folder = ?1" }) {
		Stmt s(db_, sql);
		s.text(1, folder);
		s.step();
	}
}

std::vector<bep::FileInfo> Db::versions(const std::string &folder, const std::string &name,
	std::vector<DeviceId> *devices)
{
	Stmt s(db_, "SELECT device, fi FROM files WHERE folder = ?1 AND name = ?2");
	s.text(1, folder).text(2, name);
	std::vector<bep::FileInfo> out;
	while (s.step()) {
		if (devices)
			devices->push_back(s.col_device(0));
		out.push_back(decode_row(s.col_blob(1)));
	}
	return out;
}

void Db::recompute_global(const std::string &folder, const std::string &name)
{
	std::vector<DeviceId> devices;
	const auto all = versions(folder, name, &devices);
	if (all.empty())
		return;
	size_t best = 0;
	// The local copy wins ties, so an equal remote version never makes us "need" anything.
	for (size_t i = 0; i < all.size(); i++) {
		if (i == best)
			continue;
		if (newer_than(all[i], all[best]) ||
			(vec::equal(all[i].version, all[best].version) && devices[i] == local_device() &&
				!is_invalid(all[i])))
			best = i;
	}
	const bep::FileInfo &g = all[best];
	const DeviceId local = local_device();
	bool have_local = false;
	bool local_is_current = false;
	for (size_t i = 0; i < all.size(); i++)
		if (devices[i] == local) {
			have_local = true;
			local_is_current = vec::equal(all[i].version, g.version) || (all[i].local_flags & kFlagIgnored) ||
				(all[i].local_flags & kFlagReceiveOnly);
		}
	// We need the global version if it's valid, not ours, and (we lack the name and it isn't a deletion, or ours is
	// older).
	const bool need = devices[best] != local && !is_invalid(g) &&
		(have_local ? !local_is_current : !g.deleted);
	Stmt clear(db_, "UPDATE files SET global = 0, need = 0 WHERE folder = ?1 AND name = ?2");
	clear.text(1, folder).text(2, name);
	clear.step();
	Stmt set(db_, "UPDATE files SET global = 1, need = ?4 WHERE folder = ?1 AND name = ?2 AND device = ?3");
	set.text(1, folder).text(2, name).device(3, devices[best]).i64(4, need);
	set.step();
}

std::optional<bep::FileInfo> Db::get(const std::string &folder, const DeviceId &device, const std::string &name)
{
	Stmt s(db_, "SELECT fi FROM files WHERE folder = ?1 AND device = ?2 AND name = ?3");
	s.text(1, folder).device(2, device).text(3, name);
	if (!s.step())
		return std::nullopt;
	return decode_row(s.col_blob(0));
}

std::optional<bep::FileInfo> Db::global(const std::string &folder, const std::string &name)
{
	Stmt s(db_, "SELECT fi FROM files WHERE folder = ?1 AND name = ?2 AND global = 1");
	s.text(1, folder).text(2, name);
	if (!s.step())
		return std::nullopt;
	return decode_row(s.col_blob(0));
}

std::vector<DeviceId> Db::availability(const std::string &folder, const std::string &name)
{
	const auto g = global(folder, name);
	if (!g)
		return {};
	std::vector<DeviceId> devices;
	const auto all = versions(folder, name, &devices);
	std::vector<DeviceId> out;
	for (size_t i = 0; i < all.size(); i++)
		if (devices[i] != local_device() && !is_invalid(all[i]) && vec::equal(all[i].version, g->version))
			out.push_back(devices[i]);
	return out;
}

void Db::each_local(const std::string &folder, const std::function<bool(const bep::FileInfo &)> &fn)
{
	Stmt s(db_, "SELECT fi FROM files WHERE folder = ?1 AND device = ?2 ORDER BY name");
	s.text(1, folder).device(2, local_device());
	while (s.step())
		if (!fn(decode_row(s.col_blob(0))))
			break;
}

void Db::local_since(const std::string &folder, int64_t after, const std::function<bool(const bep::FileInfo &)> &fn)
{
	Stmt s(db_, "SELECT fi FROM files WHERE folder = ?1 AND device = ?2 AND sequence > ?3 ORDER BY sequence");
	s.text(1, folder).device(2, local_device()).i64(3, after);
	while (s.step())
		if (!fn(decode_row(s.col_blob(0))))
			break;
}

void Db::each_needed(const std::string &folder, const std::function<bool(const bep::FileInfo &)> &fn)
{
	// Directories first (shallow to deep), then files, then deletions deepest first, as the puller wants them.
	Stmt s(db_, "SELECT fi FROM files WHERE folder = ?1 AND need = 1 "
				"ORDER BY deleted, is_dir DESC, CASE WHEN deleted THEN -length(name) ELSE length(name) END, name");
	s.text(1, folder);
	std::vector<bep::FileInfo> batch;
	while (s.step())
		batch.push_back(decode_row(s.col_blob(0)));
	for (const auto &f : batch)
		if (!fn(f))
			break;
}

namespace {
Db::Counts counts_from(Stmt &s)
{
	Db::Counts c;
	while (s.step()) {
		const bool deleted = s.col_i64(0) != 0;
		const bool dir = s.col_i64(1) != 0;
		const int64_t n = s.col_i64(2);
		if (deleted)
			c.deleted += n;
		else if (dir)
			c.directories += n;
		else
			c.files += n;
		if (!deleted)
			c.bytes += s.col_i64(3);
	}
	return c;
}
} // namespace

Db::Counts Db::local_counts(const std::string &folder)
{
	Stmt s(db_, "SELECT deleted, is_dir, COUNT(*), COALESCE(SUM(size), 0) FROM files "
				"WHERE folder = ?1 AND device = ?2 AND invalid = 0 GROUP BY deleted, is_dir");
	s.text(1, folder).device(2, local_device());
	return counts_from(s);
}

Db::Counts Db::global_counts(const std::string &folder)
{
	Stmt s(db_, "SELECT deleted, is_dir, COUNT(*), COALESCE(SUM(size), 0) FROM files "
				"WHERE folder = ?1 AND global = 1 AND invalid = 0 GROUP BY deleted, is_dir");
	s.text(1, folder);
	return counts_from(s);
}

Db::Counts Db::need_counts(const std::string &folder)
{
	Stmt s(db_, "SELECT deleted, is_dir, COUNT(*), COALESCE(SUM(size), 0) FROM files "
				"WHERE folder = ?1 AND need = 1 GROUP BY deleted, is_dir");
	s.text(1, folder);
	return counts_from(s);
}

Db::Counts Db::remote_need_counts(const std::string &folder, const DeviceId &device)
{
	// Global files the device lacks or has an older version of.
	Db::Counts c;
	Stmt s(db_, "SELECT g.fi, r.fi FROM files g LEFT JOIN files r ON r.folder = g.folder AND r.name = g.name AND "
				"r.device = ?2 WHERE g.folder = ?1 AND g.global = 1 AND g.invalid = 0");
	s.text(1, folder).device(2, device);
	while (s.step()) {
		const auto g = decode_row(s.col_blob(0));
		const auto rb = s.col_blob(1);
		if (rb.data() != nullptr && rb.size() > 0) {
			const auto r = decode_row(rb);
			if (vec::greater_equal(r.version, g.version))
				continue;
		} else if (g.deleted) {
			continue;
		}
		if (g.deleted)
			c.deleted++;
		else if (g.type == bep::FileInfoType::directory)
			c.directories++;
		else {
			c.files++;
			c.bytes += g.size;
		}
	}
	return c;
}

std::optional<std::string> Db::kv_get(const std::string &key)
{
	Stmt s(db_, "SELECT value FROM kv WHERE key = ?1");
	s.text(1, key);
	if (!s.step())
		return std::nullopt;
	return s.col_text(0);
}

void Db::kv_set(const std::string &key, const std::string &value)
{
	Stmt s(db_, "INSERT OR REPLACE INTO kv (key, value) VALUES (?1, ?2)");
	s.text(1, key).text(2, value);
	s.step();
}

} // namespace xst
