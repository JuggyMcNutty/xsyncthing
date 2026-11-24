// xst-console-test.xex: the on-console checks for Phase 0 and 1. Copy it to the console and launch it; it writes
// PASS/FAIL lines to xst-console-test.log beside it (xsdk names the log after the .xex), then
// "checks: <n> passed, <m> failed". No display is needed: it shows a line of status and ends by itself.
//
//   1. wolfCrypt's own test suite (AES, SHA-2, RSA, ECC, Curve25519, Ed25519, ...)
//   2. an Ed25519 identity made with the console's RNG, written as cert.pem/key.pem to hdd:/xsyncthing-test/,
//      read back, and its device ID logged
//   3. if peer.txt sits beside the .xex with "<ip> <port> <device-id>" of a stock Syncthing on the LAN: a TLS 1.3
//      handshake with it (ALPN bep/1.0, both certificates), the peer's device ID checked, and Hello exchanged.
//      For the peer to accept us, first add the device ID the log shows as a device in that Syncthing.
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <cstdio>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfcrypt/test/test.h>

#include "core/db.hpp"
#include "core/fs.hpp"
#include "core/puller.hpp"
#include "core/scanner.hpp"
#include "core/version.hpp"
#include "platform/xsdk/console.hpp"
#include "net/bep_handshake.hpp"
#include "net/identity.hpp"
#include "net/tcp.hpp"
#include "xsdk/log.h"

namespace {

uint32_t passed;
uint32_t failed;

void check(const std::string &name, bool ok)
{
	xsdk_log_write("%s: %s", ok ? "PASS" : "FAIL", name.c_str());
	passed += ok;
	failed += !ok;
}

std::string read_file(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool write_file(const std::string &path, const std::string &data)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << data;
	out.close();
	return static_cast<bool>(out);
}

void test_wolfcrypt()
{
	const auto start = std::chrono::steady_clock::now();
	const int ret = wolfcrypt_test(nullptr);
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	xsdk_log_write("wolfcrypt_test returned %d in %lld ms", ret, static_cast<long long>(ms));
	check("wolfCrypt's test suite", ret == 0);
}

std::optional<xst::Identity> test_identity()
{
	const std::string dir = "hdd:/xsyncthing-test";
	mkdir(dir.c_str(), 0777);
	std::string error;
	const auto start = std::chrono::steady_clock::now();
	auto id = xst::Identity::generate(&error);
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	check("an Ed25519 identity is generated", id.has_value());
	if (!id) {
		xsdk_log_write("identity: %s", error.c_str());
		return std::nullopt;
	}
	xsdk_log_write("generated in %lld ms; device ID %s", static_cast<long long>(ms), id->id.to_string().c_str());
	// The test's own round trip, in a scratch pair of files.
	check("cert.pem and key.pem are written to hdd:",
		write_file(dir + "/new-cert.pem", id->cert_pem()) && write_file(dir + "/new-key.pem", id->key_pem()));
	const auto back =
		xst::Identity::from_pem(read_file(dir + "/new-cert.pem"), read_file(dir + "/new-key.pem"), &error);
	check("the identity reads back with the same device ID", back && back->id == id->id);
	// A second identity must differ: the RNG isn't constant (in Xenia it is, and this fails there).
	auto other = xst::Identity::generate(&error);
	check("a second identity has a different device ID (the RNG is live)", other && other->id != id->id);

	// The identity the handshake uses stays the same from run to run (cert.pem/key.pem, made once), so a peer
	// told our device ID once keeps accepting us.
	auto kept = xst::Identity::from_pem(read_file(dir + "/cert.pem"), read_file(dir + "/key.pem"), &error);
	if (!kept) {
		write_file(dir + "/cert.pem", id->cert_pem());
		write_file(dir + "/key.pem", id->key_pem());
		kept = id;
	}
	xsdk_log_write("this console's test device ID: %s", kept->id.to_string().c_str());
	return kept;
}

void test_handshake(const xst::Identity &identity)
{
	std::istringstream peer(read_file("game:/peer.txt"));
	std::string host, device;
	int port = 0;
	if (!(peer >> host >> port >> device)) {
		xsdk_log_write("no game:/peer.txt (\"<ip> <port> <device-id>\"): skipping the handshake with a peer");
		return;
	}
	const auto expected = xst::DeviceId::parse(device);
	check("peer.txt's device ID parses", expected.has_value());
	std::string error;
	auto tcp = xst::TcpStream::connect(host, static_cast<uint16_t>(port), &error);
	check(std::format("TCP connect to {}:{}", host, port), tcp != nullptr);
	if (!tcp) {
		xsdk_log_write("%s", error.c_str());
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	auto tls = xst::TlsConnection::handshake(std::move(tcp),
		xst::bep_tls_options(identity, xst::TlsOptions::Role::client), &error);
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	check("TLS 1.3 handshake with the peer", tls != nullptr);
	if (!tls) {
		xsdk_log_write("%s", error.c_str());
		return;
	}
	xsdk_log_write("TLS in %lld ms: %s %s, ALPN %s", static_cast<long long>(ms), tls->version().c_str(),
		tls->cipher().c_str(), tls->alpn().c_str());
	check("the TLS version is 1.3", tls->version() == "TLSv1.3");
	check("ALPN is bep/1.0", tls->alpn() == xst::kBepAlpn);
	check("the peer's device ID is the one in peer.txt", expected && tls->peer_device_id() == *expected);

	xst::Hello ours;
	ours.device_name = "xbox360";
	ours.client_name = std::string(xst::client_name());
	ours.client_version = std::string(xst::client_version());
	ours.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch())
						 .count();
	const auto theirs = xst::exchange_hello(*tls, ours, &error);
	check("Hello exchanged", theirs.has_value());
	if (theirs)
		xsdk_log_write("peer Hello: device \"%s\", client %s %s", theirs->device_name.c_str(),
			theirs->client_name.c_str(), theirs->client_version.c_str());
	else
		xsdk_log_write("hello: %s", error.c_str());
}

// Phase 3: the index database through our SQLite VFS, the volume rules, and a scan on the hard disk.
void test_storage()
{
	const std::string dir = "hdd:/xsyncthing-test";
	std::string error;
	::remove((dir + "/index.db").c_str());
	auto db = xst::Db::open(dir + "/index.db", &error);
	check("SQLite opens a database on hdd: through the xsdk VFS", db != nullptr);
	if (!db) {
		xsdk_log_write("db: %s", error.c_str());
		return;
	}
	xst::bep::FileInfo f;
	f.name = "a/b.txt";
	f.size = 3;
	f.sequence = 1;
	f.version.counters = { { 1, 1 } };
	db->begin();
	db->update("t", xst::Db::local_device(), { f });
	db->commit();
	const auto back = db->get("t", xst::Db::local_device(), "a/b.txt");
	check("a file record round-trips through the database", back && back->size == 3 && back->version == f.version);
	db.reset();
	auto again = xst::Db::open(dir + "/index.db", &error);
	check("the database reopens with the record", again && again->get("t", xst::Db::local_device(), "a/b.txt"));
	again.reset();

	const auto rules = xst::fs::console_rules("hdd:/xsyncthing-test");
	xsdk_log_write("hdd: is %s, names up to %u bytes", rules.type.c_str(), (unsigned)rules.max_name_bytes);
	check("the hard disk is FATX or exFAT", rules.type == "fatx" || rules.type == "exfat");
	if (rules.type == "fatx")
		check("FATX refuses a 43-byte name", !xst::fs::unstorable_reason(std::string(43, 'n'), 1, rules).empty());

	// Scan a small folder on the hard disk.
	const std::string folder = dir + "/scan";
	auto fsys = xst::fs::open_console(folder);
	fsys->mkdir_all("");
	fsys->mkdir_all("sub");
	if (auto out = fsys->create("sub/data.bin")) {
		std::vector<uint8_t> data(300 * 1024);
		for (size_t i = 0; i < data.size(); i++)
			data[i] = static_cast<uint8_t>(i * 7);
		out->write_at(data, 0);
		out->close();
	}
	::remove((dir + "/scan.db").c_str());
	auto sdb = xst::Db::open(dir + "/scan.db", &error);
	xst::IgnoreMatcher ignores;
	xst::ScanOptions o;
	o.folder_id = "scan";
	o.short_id = 0x1234;
	o.ignore_permissions = true;
	o.mtime_window_ns = rules.mtime_window_ns;
	const auto start = std::chrono::steady_clock::now();
	const auto r = xst::scan(*fsys, *sdb, ignores, o);
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	xsdk_log_write("scan: %lld changed, %lld bytes hashed in %lld ms", (long long)r.changed,
		(long long)r.hashed_bytes, (long long)ms);
	const auto rec = sdb->get("scan", xst::Db::local_device(), "sub/data.bin");
	check("a scan on hdd: hashes the file into 3 blocks", rec && rec->blocks.size() == 3 && rec->size == 300 * 1024);
	const auto r2 = xst::scan(*fsys, *sdb, ignores, o);
	check("a second scan finds nothing changed (mtime window)", r2.changed == 0);

	// Rename over an existing file (the puller's last step): xsdk can't rename over, the fs layer removes first.
	if (auto t = fsys->create(".syncthing.x.tmp")) {
		const uint8_t one = 1;
		t->write_at(std::span(&one, 1), 0);
		t->close();
	}
	if (auto t = fsys->create("x")) {
		t->close();
	}
	check("rename replaces an existing file", fsys->rename(".syncthing.x.tmp", "x") && fsys->stat("x")->size == 1);
}

// Every drive xsyncthing can keep folders on: its type and rules, and a write-read-rename where it's writable.
void test_volumes()
{
	for (const char *drive : { "hdd:", "usb0:", "usb1:", "usb2:" }) {
		struct stat st;
		if (::stat((std::string(drive) + "/").c_str(), &st) != 0) {
			xsdk_log_write("volume %s: not present", drive);
			continue;
		}
		const std::string dir = std::string(drive) + "/xst-volume-test";
		const auto rules = xst::fs::console_rules(dir);
		xsdk_log_write("volume %s: %s, names <= %u bytes, files <= %lld bytes, mtime step %lld ns", drive,
			rules.type.c_str(), (unsigned)rules.max_name_bytes, (long long)rules.max_file_size,
			(long long)rules.mtime_window_ns);
		auto fsys = xst::fs::open_console(dir);
		if (!fsys->mkdir_all("")) {
			xsdk_log_write("volume %s: read-only or full (%s)", drive, fsys->last_error().c_str());
			continue;
		}
		const std::string longest(rules.max_name_bytes - 4, 'n');
		bool ok = false;
		if (auto f = fsys->create(longest + ".txt")) {
			const uint8_t d[3] = { 1, 2, 3 };
			ok = f->write_at(d, 0) == 3;
			f->close();
		}
		ok = ok && fsys->rename(longest + ".txt", "renamed.txt") && fsys->stat("renamed.txt") &&
			fsys->stat("renamed.txt")->size == 3 && fsys->remove("renamed.txt");
		check(std::string("volume ") + drive + " (" + rules.type + "): the longest name it takes writes, renames, removes",
			ok);
		fsys->remove("");
	}
}

} // namespace

int main()
{
	xsdk_log_write("xst-console-test: %s %s", std::string(xst::client_name()).c_str(),
		std::string(xst::client_version()).c_str());
	if (!std::ifstream("game:/skip-wolfcrypt.txt"))
		test_wolfcrypt();
	if (auto id = test_identity())
		test_handshake(*id);
	test_storage();
	test_volumes();
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed);
	return failed ? 1 : 0;
}
