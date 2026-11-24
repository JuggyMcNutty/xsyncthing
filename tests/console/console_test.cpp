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

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfcrypt/test/test.h>

#include "core/version.hpp"
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
	check("cert.pem and key.pem are written to hdd:",
		write_file(dir + "/cert.pem", id->cert_pem()) && write_file(dir + "/key.pem", id->key_pem()));
	const auto back = xst::Identity::from_pem(read_file(dir + "/cert.pem"), read_file(dir + "/key.pem"), &error);
	check("the identity reads back with the same device ID", back && back->id == id->id);
	// A second identity must differ: the RNG isn't constant (in Xenia it is, and this fails there).
	auto other = xst::Identity::generate(&error);
	check("a second identity has a different device ID (the RNG is live)", other && other->id != id->id);
	return id;
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

} // namespace

int main()
{
	xsdk_log_write("xst-console-test: %s %s", std::string(xst::client_name()).c_str(),
		std::string(xst::client_version()).c_str());
	test_wolfcrypt();
	if (auto id = test_identity())
		test_handshake(*id);
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed, (unsigned)failed);
	return failed ? 1 : 0;
}
