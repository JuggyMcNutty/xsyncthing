// xst-handshake: the Phase 1 spike on the host. Loads (or makes) an identity, dials a Syncthing device, runs the
// TLS 1.3 + bep/1.0 handshake, checks the peer's device ID, and exchanges Hello.
//   xst-handshake <dir> <host> <port> [expected-device-id]
// <dir> holds cert.pem and key.pem; they're generated if missing.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/version.hpp"
#include "net/bep_connection.hpp"
#include "net/bep_handshake.hpp"
#include "net/identity.hpp"
#include "net/tcp.hpp"

namespace {

std::string read_file(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool write_file(const std::string &path, const std::string &data)
{
	std::ofstream out(path, std::ios::binary);
	out << data;
	return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "usage: %s <dir> <host> <port> [expected-device-id]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1];
	std::string error;
	std::optional<xst::Identity> identity;
	const std::string cert = read_file(dir + "/cert.pem");
	const std::string key = read_file(dir + "/key.pem");
	if (!cert.empty() && !key.empty()) {
		identity = xst::Identity::from_pem(cert, key, &error);
	} else {
		identity = xst::Identity::generate(&error);
		if (identity) {
			write_file(dir + "/cert.pem", identity->cert_pem());
			write_file(dir + "/key.pem", identity->key_pem());
		}
	}
	if (!identity) {
		std::fprintf(stderr, "identity: %s\n", error.c_str());
		return 1;
	}
	std::printf("our device ID:  %s\n", identity->id.to_string().c_str());

	auto tcp = xst::TcpStream::connect(argv[2], static_cast<uint16_t>(std::atoi(argv[3])), &error);
	if (!tcp) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	auto tls = xst::TlsConnection::handshake(std::move(tcp),
		xst::bep_tls_options(*identity, xst::TlsOptions::Role::client), &error);
	if (!tls) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::printf("TLS:            %s %s, ALPN %s\n", tls->version().c_str(), tls->cipher().c_str(),
		tls->alpn().c_str());
	const auto peer = tls->peer_device_id();
	std::printf("peer device ID: %s\n", peer.to_string().c_str());
	if (argc > 4) {
		const auto expected = xst::DeviceId::parse(argv[4]);
		if (!expected || *expected != peer) {
			std::fprintf(stderr, "the peer's device ID isn't the one expected\n");
			return 1;
		}
	}

	xst::Hello ours;
	ours.device_name = "xsyncthing-handshake";
	ours.client_name = std::string(xst::client_name());
	ours.client_version = std::string(xst::client_version());
	ours.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch())
						 .count();
	const auto theirs = xst::exchange_hello(*tls, ours, &error);
	if (!theirs) {
		std::fprintf(stderr, "hello: %s\n", error.c_str());
		return 1;
	}
	std::printf("peer Hello:     device \"%s\", client %s %s, %d connections\n", theirs->device_name.c_str(),
		theirs->client_name.c_str(), theirs->client_version.c_str(), theirs->num_connections);

	// Then BEP proper: send an empty ClusterConfig and print what the peer sends for a few messages.
	xst::BepConnection bep(std::move(tls), xst::bep::CompressionPolicy::metadata);
	if (!bep.send(xst::bep::ClusterConfig{})) {
		std::fprintf(stderr, "sending ClusterConfig failed\n");
		return 1;
	}
	for (int i = 0; i < 1; i++) {
		const auto message = bep.receive(&error);
		if (!message) {
			std::fprintf(stderr, "receive: %s\n", error.c_str());
			return 1;
		}
		if (const auto *cc = std::get_if<xst::bep::ClusterConfig>(&*message)) {
			std::printf("peer ClusterConfig: %zu folders\n", cc->folders.size());
			for (const auto &f : cc->folders)
				std::printf("  folder %s \"%s\" (%zu devices)\n", f.id.c_str(), f.label.c_str(), f.devices.size());
			bep.send(xst::bep::Ping{});
		} else if (const auto *idx = std::get_if<xst::bep::Index>(&*message)) {
			std::printf("peer Index for %s: %zu files\n", idx->folder.c_str(), idx->files.size());
			for (const auto &f : idx->files)
				std::printf("  %s (%lld bytes, %zu blocks)\n", f.name.c_str(), static_cast<long long>(f.size),
					f.blocks.size());
		} else {
			std::printf("peer message type %d\n", static_cast<int>(xst::bep::type_of(*message)));
		}
		if (i == 0 && argc > 5)
			break;
	}
	bep.close("xst-handshake done");
	return 0;
}
