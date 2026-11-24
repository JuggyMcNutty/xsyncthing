// The scanner, the index database and the puller together, on two temporary folders standing in for two devices:
// device A scans, its index goes to B's database as a remote device, B pulls (blocks fetched from A's files).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "core/db.hpp"
#include "core/puller.hpp"
#include "core/scanner.hpp"
#include "core/vector.hpp"
#include "test.hpp"

using namespace xst;
namespace sfs = std::filesystem;

namespace {

struct Side {
	sfs::path dir;
	std::unique_ptr<fs::Filesystem> fs;
	std::unique_ptr<Db> db;
	uint64_t short_id;
	DeviceId id;
	IgnoreMatcher ignores;
};

Side make_side(const std::string &name, uint64_t short_id)
{
	Side s;
	s.dir = sfs::temp_directory_path() / ("xst-sync-" + name + "-" + std::to_string(::getpid()));
	sfs::remove_all(s.dir);
	sfs::create_directories(s.dir / "data");
	s.fs = fs::open_posix((s.dir / "data").string());
	std::string error;
	s.db = Db::open((s.dir / "index.db").string(), &error);
	s.short_id = short_id;
	DeviceId::Bytes b{};
	for (int i = 0; i < 8; i++)
		b[i] = static_cast<uint8_t>(short_id >> (56 - 8 * i));
	s.id = DeviceId(b);
	return s;
}

void write(const Side &s, const std::string &name, const std::string &content)
{
	sfs::create_directories((s.dir / "data" / name).parent_path());
	std::ofstream(s.dir / "data" / name, std::ios::binary) << content;
}

std::string read(const Side &s, const std::string &name)
{
	std::ifstream in(s.dir / "data" / name, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void scan_side(Side &s)
{
	ScanOptions o;
	o.folder_id = "f";
	o.short_id = s.short_id;
	scan(*s.fs, *s.db, s.ignores, o);
}

// Copies from's local index into to's database as from's device.
void exchange(Side &from, Side &to)
{
	std::vector<bep::FileInfo> files;
	from.db->each_local("f", [&](const bep::FileInfo &f) {
		files.push_back(f);
		return true;
	});
	to.db->begin();
	to.db->update("f", from.id, files);
	to.db->commit();
}

PullResult pull_side(Side &to, Side &from)
{
	PullOptions o;
	o.folder_id = "f";
	o.short_id = to.short_id;
	o.conflict_timestamp = [] { return std::string("20260124-101500"); };
	BlockFetcher fetch = [&](const std::vector<DeviceId> &, const bep::FileInfo &f, const bep::BlockInfo &b, int) {
		auto file = from.fs->open_read(f.name);
		std::optional<bep::Bytes> out;
		if (!file)
			return out;
		bep::Bytes buf(static_cast<size_t>(b.size));
		if (file->read_at(buf, b.offset) == b.size)
			out = std::move(buf);
		return out;
	};
	return pull(*to.fs, *to.db, to.ignores, fetch, o);
}

} // namespace

TEST(sync_new_files_and_dirs)
{
	Side a = make_side("a1", 0x1111111111111111);
	Side b = make_side("b1", 0x2222222222222222);
	write(a, "hello.txt", "hello world");
	write(a, "dir/sub/big.bin", std::string(300 * 1024, 'x') + "tail");
	write(a, "empty", "");
	scan_side(a);
	exchange(a, b);
	const auto r = pull_side(b, a);
	CHECK(r.errors.empty());
	CHECK_EQ(read(b, "hello.txt"), "hello world");
	CHECK_EQ(read(b, "dir/sub/big.bin").size(), 300u * 1024 + 4);
	CHECK(sfs::exists(b.dir / "data" / "empty"));
	// Nothing left to pull, and B's version equals A's.
	int needed = 0;
	b.db->each_needed("f", [&](const bep::FileInfo &) {
		needed++;
		return true;
	});
	CHECK_EQ(needed, 0);
	// A 300 KiB file of 'x' has two identical 128 KiB blocks: the second is reused locally.
	CHECK(r.bytes_reused > 0);
}

TEST(sync_modify_and_delete)
{
	Side a = make_side("a2", 0x1111111111111111);
	Side b = make_side("b2", 0x2222222222222222);
	write(a, "x.txt", "one");
	write(a, "gone.txt", "bye");
	scan_side(a);
	exchange(a, b);
	pull_side(b, a);
	scan_side(b);
	exchange(b, a);

	write(a, "x.txt", "two, longer");
	sfs::remove(a.dir / "data" / "gone.txt");
	scan_side(a);
	exchange(a, b);
	const auto r = pull_side(b, a);
	CHECK(r.errors.empty());
	CHECK_EQ(read(b, "x.txt"), "two, longer");
	CHECK(!sfs::exists(b.dir / "data" / "gone.txt"));
	const auto g = b.db->global("f", "gone.txt");
	CHECK(g && g->deleted);
}

TEST(sync_conflict_keeps_both)
{
	Side a = make_side("a3", 0x1111111111111111);
	Side b = make_side("b3", 0x2222222222222222);
	write(a, "c.txt", "base");
	scan_side(a);
	exchange(a, b);
	pull_side(b, a);
	scan_side(b);

	// Both edit it independently.
	write(a, "c.txt", "from A");
	write(b, "c.txt", "from B, edited later");
	scan_side(a);
	scan_side(b);
	// Make B's edit clearly older so A's wins on B's side.
	{
		auto bf = *b.db->get("f", Db::local_device(), "c.txt");
		auto af = *a.db->get("f", Db::local_device(), "c.txt");
		CHECK(vec::concurrent(af.version, bf.version));
		af.modified_s = bf.modified_s + 100;
		a.db->begin();
		a.db->update("f", Db::local_device(), { af });
		a.db->commit();
	}
	exchange(a, b);
	const auto r = pull_side(b, a);
	CHECK(r.errors.empty());
	CHECK_EQ(r.conflicts, 1);
	CHECK_EQ(read(b, "c.txt"), "from A");
	CHECK_EQ(read(b, "c.sync-conflict-20260124-101500-EIRCEIR.txt"), "from B, edited later");
}

TEST(sync_ignored_not_pulled)
{
	Side a = make_side("a4", 0x1111111111111111);
	Side b = make_side("b4", 0x2222222222222222);
	write(a, "keep.txt", "k");
	write(a, "skip.log", "s");
	scan_side(a);
	exchange(a, b);
	std::string error;
	CHECK(b.ignores.parse("*.log\n", nullptr, &error));
	pull_side(b, a);
	CHECK(sfs::exists(b.dir / "data" / "keep.txt"));
	CHECK(!sfs::exists(b.dir / "data" / "skip.log"));
}

TEST(temp_and_conflict_names)
{
	CHECK_EQ(temp_name("d/file.txt", 255), "d/.syncthing.file.txt.tmp");
	const auto t = temp_name("d/a-name-that-is-long-enough-to-overflow.bin", 42);
	CHECK(t.starts_with("d/.st~") && t.size() <= 2 + 42);
	CHECK_EQ(conflict_name("a/b.tar.gz", "20260101-000000", "ABCDEFG"), "a/b.tar.sync-conflict-20260101-000000-ABCDEFG.gz");
	CHECK_EQ(conflict_name("noext", "20260101-000000", "ABCDEFG"), "noext.sync-conflict-20260101-000000-ABCDEFG");
	CHECK(is_conflict_name("x/y.sync-conflict-20260101-000000-ABCDEFG.txt"));
}

TEST(fatx_unstorable)
{
	fs::VolumeRules fatx;
	fatx.type = "fatx";
	fatx.max_name_bytes = 42;
	fatx.max_file_size = 0xFFFFFFFFLL;
	fatx.forbidden_chars = "<>:\"\\|?*";
	CHECK(fs::unstorable_reason("ok/name.txt", 10, fatx).empty());
	CHECK(!fs::unstorable_reason(std::string(43, 'a'), 10, fatx).empty());
	CHECK(!fs::unstorable_reason("a?b", 10, fatx).empty());
	CHECK(!fs::unstorable_reason("big", 5LL << 30, fatx).empty());
	CHECK(!fs::unstorable_reason("trailing.", 1, fatx).empty());
}

TEST(block_sizes)
{
	CHECK_EQ(block_size_for(0), 128 << 10);
	CHECK_EQ(block_size_for((250LL << 20) - 1), 128 << 10); // < 2000 * 128 KiB
	CHECK_EQ(block_size_for(250LL << 20), 256 << 10);
	CHECK_EQ(block_size_for(1LL << 40), 16 << 20);
}

TEST(sync_unstorable_name_is_never_deleted_remotely)
{
	// Regression (console run 2026-11-24): a name FATX can't hold was recorded as unsupported, then the next scan
	// saw it missing from disk and announced a deletion, which deleted it on the PC.
	Side a = make_side("a5", 0x1111111111111111);
	Side b = make_side("b5", 0x2222222222222222);
	fs::VolumeRules fatx;
	fatx.type = "fatx";
	fatx.max_name_bytes = 42;
	fatx.keeps_permissions = false;
	b.fs = fs::open_posix_with_rules((b.dir / "data").string(), fatx);
	const std::string long_name = "this-file-name-is-much-too-long-for-the-xbox-fatx-volume.txt";
	write(a, long_name, "keep me");
	write(a, "short.txt", "fine");
	scan_side(a);
	exchange(a, b);
	const auto r = pull_side(b, a);
	CHECK_EQ(r.errors.size(), 1u);
	CHECK(r.errors.count(long_name));
	CHECK_EQ(read(b, "short.txt"), "fine");
	scan_side(b); // must not turn the unstored name into a deletion
	const auto mine = b.db->get("f", Db::local_device(), long_name);
	CHECK(mine && !mine->deleted && (mine->local_flags & kFlagUnsupported));
	exchange(b, a);
	const auto g = a.db->global("f", long_name);
	CHECK(g && !g->deleted);
	// And nothing is left for B to pull over and over.
	int needed = 0;
	b.db->each_needed("f", [&](const bep::FileInfo &) {
		needed++;
		return true;
	});
	CHECK_EQ(needed, 0);
}
