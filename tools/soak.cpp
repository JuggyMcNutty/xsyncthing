// xst-soak: scan and pull timings for a large folder, with memory use, on the host or (as a title) on the console.
//   xst-soak <dir> <files> [file-bytes]
// Makes <files> files of [file-bytes] (default 1 KiB) across 1000 directories under <dir>/src, scans them, rescans
// (nothing changed), then pulls the whole index into <dir>/dst (blocks copied from src, as from a peer).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(__XSDK__)
#include <sys/resource.h>
#else
#include <malloc.h>
#endif

#include "core/db.hpp"
#include "core/fs.hpp"
#include "core/puller.hpp"
#include "core/scanner.hpp"
#include "platform/platform.hpp"
#include "platform/progress.hpp"

using namespace xst;
using clk = std::chrono::steady_clock;

namespace {
// Results go to stdout (the title's log), the on-screen log, and the screen's list of finished phases.
template <class... A> void report(const char *fmt, A... args)
{
	char buf[512];
	std::snprintf(buf, sizeof buf, fmt, args...);
	std::string line = buf;
	while (!line.empty() && line.back() == '\n')
		line.pop_back();
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
	progress::note(line);
}

double since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }
long rss_mib()
{
#if defined(__XSDK__)
	// The heap's high-water mark from newlib's mallinfo.
	return static_cast<long>(mallinfo().arena >> 20);
#else
	rusage u{};
	getrusage(RUSAGE_SELF, &u);
	return u.ru_maxrss / 1024;
#endif
}
} // namespace

#if defined(__XSDK__)
#define main soak_main
#include "xsdk/log.h"
// On the console, stdout goes to the title's log already (xsdk docs/GUIDE.md); memory comes from the kernel.
#endif

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s <dir> <files> [bytes]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1];
	const int n = std::atoi(argv[2]);
	const int bytes = argc > 3 ? std::atoi(argv[3]) : 1024;
	const bool resume = argc > 4 && std::string(argv[4]) == "resume";
#if defined(__XSDK__)
	auto src = fs::open_console(dir + "/src");
	auto dst = fs::open_console(dir + "/dst");
#else
	auto src = fs::open_posix(dir + "/src");
	auto dst = fs::open_posix(dir + "/dst");
#endif
	src->mkdir_all("");
	dst->mkdir_all("");
	std::string error;
	if (resume) {
		// After a crash or power loss mid-pull: reopen both databases as they were, finish the pull, then re-hash
		// every pulled file (a fresh scan, which also removes leftover temp files) and compare with the source.
		auto sdb = Db::open(dir + "/src.db", &error);
		auto ddb = sdb ? Db::open(dir + "/dst.db", &error) : nullptr;
		report("resume: databases reopen: %s\n", ddb ? "yes" : error.c_str());
		if (!ddb)
			return 1;
		int before = 0;
		ddb->each_needed("soak", [&](const bep::FileInfo &) {
			before++;
			return true;
		});
		report("resume: %d files still needed after the interruption\n", before);
		PullOptions po;
		po.folder_id = "soak";
		po.short_id = 2;
		po.ignore_permissions = !dst->rules().keeps_permissions;
		po.max_name_bytes = dst->rules().max_name_bytes;
#if defined(__XSDK__)
		po.fsync = !std::ifstream("game:/nofsync.txt");
#endif
		BlockFetcher fetch = [&](const std::vector<DeviceId> &, const bep::FileInfo &f, const bep::BlockInfo &blk, int) {
			std::optional<bep::Bytes> out;
			if (auto file = src->open_read(f.name)) {
				bep::Bytes buf(static_cast<size_t>(blk.size));
				if (file->read_at(buf, blk.offset) == blk.size)
					out = std::move(buf);
			}
			return out;
		};
		progress::set_title("xsyncthing soak: resuming in " + dir);
		progress::phase("finishing the interrupted pull", before);
		po.on_pulled = [](const bep::FileInfo &) { progress::advance(); };
		auto t = clk::now();
		const auto pr = pull(*dst, *ddb, IgnoreMatcher(), fetch, po);
		report("resume: pulled %lld files in %.1f s (%.0f files/s), %zu errors; %lld conflict copies\n",
			(long long)pr.pulled, since(t), pr.pulled / std::max(0.001, since(t)), pr.errors.size(), (long long)pr.conflicts);
		int left = 0;
		ddb->each_needed("soak", [&](const bep::FileInfo &) {
			left++;
			return true;
		});
		report("resume: left to pull: %d\n", left);

		// Verify: a fresh scan of the destination (temp files removed), compared file by file with the source index.
		std::remove((dir + "/verify.db").c_str());
		auto vdb = Db::open(dir + "/verify.db", &error);
		ScanOptions vo;
		vo.folder_id = "soak";
		vo.short_id = 3;
		vo.ignore_permissions = true;
		vo.mtime_window_ns = dst->rules().mtime_window_ns;
		vo.temp_lifetime_s = 0;
		int64_t temps = 0;
		vo.temps_removed = &temps;
		progress::phase("verifying: re-hashing every pulled file", n);
		vo.on_file = [] { progress::advance(); };
		t = clk::now();
		IgnoreMatcher none;
		scan(*dst, *vdb, none, vo);
		report("verify: destination re-hashed in %.1f s; %lld leftover temp files removed\n", since(t),
			(long long)temps);
		int64_t same = 0, wrong = 0, missing = 0;
		sdb->each_local("soak", [&](const bep::FileInfo &f) {
			if (f.deleted || f.type != bep::FileInfoType::file)
				return true;
			const auto d = vdb->get("soak", Db::local_device(), f.name);
			if (!d || d->deleted)
				missing++;
			else if (d->blocks_hash == f.blocks_hash && d->size == f.size)
				same++;
			else
				wrong++;
			return true;
		});
		int64_t extra = 0;
		vdb->each_local("soak", [&](const bep::FileInfo &f) {
			if (f.type == bep::FileInfoType::file && !sdb->get("soak", Db::local_device(), f.name))
				extra++;
			return true;
		});
		report("verify: %lld identical, %lld different, %lld missing, %lld extra (e.g. conflict copies)\n",
			(long long)same, (long long)wrong, (long long)missing, (long long)extra);
		const bool ok = left == 0 && wrong == 0 && missing == 0 && extra == 0;
		progress::finish(ok);
		return ok ? 0 : 1;
	}
	std::remove((dir + "/src.db").c_str());
	std::remove((dir + "/dst.db").c_str());
	auto sdb = Db::open(dir + "/src.db", &error);
	auto ddb = Db::open(dir + "/dst.db", &error);

	progress::set_title("xsyncthing soak: " + std::to_string(n) + " files in " + dir);
	auto t = clk::now();
	std::string payload(static_cast<size_t>(bytes), 'x');
	// The destination starts empty (a pull into earlier results would make conflict copies).
	{
		std::vector<std::string> stack = { "" };
		std::vector<std::string> dirs;
		while (!stack.empty()) {
			const std::string d = stack.back();
			stack.pop_back();
			if (auto entries = dst->list(d))
				for (const auto &e : *entries) {
					const std::string p = fs::join(d, e.name);
					if (e.stat.kind == fs::Kind::directory) {
						stack.push_back(p);
						dirs.push_back(p);
					} else {
						dst->remove(p);
					}
				}
		}
		for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
			dst->remove(*it);
		report("cleared the destination in %.1f s\n", since(t));
		t = clk::now();
	}
	// At most about 100 entries a directory (FATX searches directories linearly): d<i/10000>/e<i/100 % 100>/f<i>.bin.
	progress::phase("creating source files", n);
	int made = 0;
	for (int i = 0; i < n; i++) {
		progress::set_done(i);
		const std::string d = "d" + std::to_string(i / 10000) + "/e" + std::to_string(i / 100 % 100);
		if (i % 100 == 0)
			src->mkdir_all(d);
		payload[0] = static_cast<char>(i);
		payload[1] = static_cast<char>(i >> 8);
		payload[2] = static_cast<char>(i >> 16);
		const std::string name = d + "/f" + std::to_string(i) + ".bin";
		if (src->stat(name))
			continue;
		auto f = src->create(name);
		f->write_at(std::span(reinterpret_cast<const uint8_t *>(payload.data()), payload.size()), 0);
		f->close();
		made++;
	}
	report("created %d files (of %d) in %.1f s\n", made, n, since(t));

	// The cost of each step the puller takes per file, over 200 files (where the time goes on a slow volume).
	{
		auto ops = fs::open_posix(dir + "/ops");
#if defined(__XSDK__)
		ops = fs::open_console(dir + "/ops");
#endif
		ops->mkdir_all("");
		const int k = 200;
		double t_create = 0, t_write = 0, t_sync = 0, t_close = 0, t_mtime = 0, t_stat = 0, t_rename = 0;
		for (int i = 0; i < k; i++) {
			const std::string tmp = ".syncthing.f" + std::to_string(i) + ".tmp", fin = "f" + std::to_string(i);
			auto a = clk::now();
			auto f = ops->create(tmp);
			t_create += since(a);
			a = clk::now();
			f->write_at(std::span(reinterpret_cast<const uint8_t *>(payload.data()), payload.size()), 0);
			t_write += since(a);
			a = clk::now();
			f->sync();
			t_sync += since(a);
			a = clk::now();
			f->close();
			t_close += since(a);
			a = clk::now();
			ops->set_mtime(tmp, 1700000000, 0);
			t_mtime += since(a);
			a = clk::now();
			ops->stat(fin);
			t_stat += since(a);
			a = clk::now();
			ops->rename(tmp, fin);
			t_rename += since(a);
		}
		report("per file (ms): create %.1f  write %.1f  fsync %.1f  close %.1f  set mtime %.1f  stat %.1f  rename %.1f\n",
			1000 * t_create / k, 1000 * t_write / k, 1000 * t_sync / k, 1000 * t_close / k, 1000 * t_mtime / k,
			1000 * t_stat / k, 1000 * t_rename / k);
	}

	// Lookup cost against directory size: stat and open a file through the source's 1000-entry root, against the
	// same in the 200-entry ops directory. (bench.txt beside the title, or XST_BENCH: only this, then exit.)
	{
		const int k = 200;
		auto ops = fs::open_posix(dir + "/ops");
#if defined(__XSDK__)
		ops = fs::open_console(dir + "/ops");
#endif
		double t_stat_big = 0, t_open_big = 0, t_stat_small = 0, t_open_small = 0, t_list_root = 0;
		for (int i = 0; i < k; i++) {
			const int j = (i * 37) % n;
			const std::string big = "d" + std::to_string(j / 10000) + "/e" + std::to_string(j / 100 % 100) + "/f" +
				std::to_string(j) + ".bin";
			const std::string small = "f" + std::to_string(i);
			auto a = clk::now();
			src->stat(big);
			t_stat_big += since(a);
			a = clk::now();
			if (auto f = src->open_read(big))
				f->close();
			t_open_big += since(a);
			a = clk::now();
			ops->stat(small);
			t_stat_small += since(a);
			a = clk::now();
			if (auto f = ops->open_read(small))
				f->close();
			t_open_small += since(a);
		}
		auto a = clk::now();
		src->list("");
		t_list_root = since(a);
		report("lookup (ms): stat through 100-entry dirs %.1f, open %.1f; stat in a 200-entry dir %.1f, open %.1f; "
					"listing the root %.0f\n",
			1000 * t_stat_big / k, 1000 * t_open_big / k, 1000 * t_stat_small / k, 1000 * t_open_small / k,
			1000 * t_list_root);
#if defined(__XSDK__)
		if (std::ifstream("game:/bench.txt"))
			return 0;
#else
		if (std::getenv("XST_BENCH"))
			return 0;
#endif
	}

	IgnoreMatcher ign;
	ScanOptions o;
	o.folder_id = "soak";
	o.short_id = 1;
	o.ignore_permissions = !src->rules().keeps_permissions;
	o.mtime_window_ns = src->rules().mtime_window_ns;
	progress::phase("scanning (hashing every file)", n);
	o.on_file = [] { progress::advance(); };
	t = clk::now();
	auto r = scan(*src, *sdb, ign, o);
	const double scan_s = since(t);
	report("scan: %lld changed in %.1f s (%.0f files/s, %.1f MiB/s hashed), %lld MiB hashed; max RSS %ld MiB\n",
		(long long)r.changed, scan_s, r.changed / scan_s, r.hashed_bytes / scan_s / (1 << 20),
		(long long)(r.hashed_bytes >> 20), rss_mib());
	progress::phase("rescanning (nothing changed)", n);
	t = clk::now();
	r = scan(*src, *sdb, ign, o);
	report("rescan (unchanged): %lld changed in %.1f s; max RSS %ld MiB\n", (long long)r.changed, since(t), rss_mib());

	// Hand the index to the destination as a remote device's, then pull it.
	t = clk::now();
	// In batches of 1000, as Index messages arrive from a peer.
	DeviceId::Bytes b{};
	b[0] = 1;
	const DeviceId peer(b);
	std::vector<bep::FileInfo> batch;
	size_t received = 0;
	ddb->begin();
	sdb->each_local("soak", [&](const bep::FileInfo &f) {
		batch.push_back(f);
		if (batch.size() == 1000) {
			ddb->update("soak", peer, batch);
			received += batch.size();
			batch.clear();
		}
		return true;
	});
	ddb->update("soak", peer, batch);
	received += batch.size();
	ddb->commit();
	report("index of %zu files received in %.1f s; max RSS %ld MiB\n", received, since(t), rss_mib());

	PullOptions po;
	po.folder_id = "soak";
	po.short_id = 2;
	po.ignore_permissions = !dst->rules().keeps_permissions;
	po.max_name_bytes = dst->rules().max_name_bytes;
	po.fsync = std::getenv("XST_NO_FSYNC") == nullptr;
#if defined(__XSDK__)
	po.fsync = !std::ifstream("game:/nofsync.txt");
#endif
	report("pull with fsync: %s\n", po.fsync ? "yes" : "no");
	PullOptions::Timing timing;
	po.timing = &timing;
	BlockFetcher fetch = [&](const std::vector<DeviceId> &, const bep::FileInfo &f, const bep::BlockInfo &blk, int) {
		std::optional<bep::Bytes> out;
		if (auto file = src->open_read(f.name)) {
			bep::Bytes buf(static_cast<size_t>(blk.size));
			if (file->read_at(buf, blk.offset) == blk.size)
				out = std::move(buf);
		}
		return out;
	};
	progress::phase("pulling into the destination", n);
	po.on_pulled = [](const bep::FileInfo &) { progress::advance(); };
	t = clk::now();
	const auto pr = pull(*dst, *ddb, ign, fetch, po);
	const double pull_s = since(t);
	report("pull: %lld files in %.1f s (%.0f files/s, %.1f MiB/s), %zu errors; max RSS %ld MiB\n",
		(long long)pr.pulled, pull_s, pr.pulled / pull_s, (pr.bytes_fetched + pr.bytes_reused) / pull_s / (1 << 20),
		pr.errors.size(), rss_mib());
	report("pull time: database %.1f s, needed-file pages %.1f s, fetching blocks %.1f s, filesystem+other %.1f s\n",
		timing.db, timing.needed_query, timing.fetch, timing.total - timing.db - timing.needed_query - timing.fetch);
	const double k = 1000.0 / std::max<int64_t>(1, pr.pulled);
	report("per pulled file (ms): mkdir %.1f create %.1f write %.1f truncate %.1f fsync %.1f close %.1f mtime %.1f "
				"stat %.1f rename %.1f fetch %.1f db %.1f\n",
		timing.mkdir * k, timing.create * k, timing.write * k, timing.truncate * k, timing.sync * k, timing.close * k,
		timing.mtime * k, timing.stat * k, timing.rename * k, timing.fetch * k, timing.db * k);
	int needed = 0;
	ddb->each_needed("soak", [&](const bep::FileInfo &) {
		needed++;
		return true;
	});
	report("left to pull: %d\n", needed);
	progress::finish(needed == 0);
	return needed ? 1 : 0;
}
