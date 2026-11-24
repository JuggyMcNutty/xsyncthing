#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/device_id.hpp"
#include "net/identity.hpp"

namespace xst {

// A byte stream TLS runs over: a TCP socket, or later a relayed session. Each returns bytes moved, 0 at end of stream,
// or -1 on error.
class Stream {
public:
	virtual ~Stream() = default;
	virtual long read(std::span<uint8_t> buffer) = 0;
	virtual long write(std::span<const uint8_t> data) = 0;
	virtual void close() = 0;
	// Wakes a read or write blocked in another thread (which then fails) without releasing the stream; safe to call
	// from any thread while the stream is in use.
	virtual void interrupt() {}
};

struct TlsOptions {
	enum class Role { client, server } role = Role::client;
	// The lowest version allowed: 13 for BEP (Syncthing requires TLS 1.3), 12 for relays and discovery.
	int min_version = 13;
	// ALPN protocols to offer (client) or accept (server); empty for none. BEP uses "bep/1.0".
	std::vector<std::string> alpn;
	// Present our certificate (BEP and relays do, both ways).
	const Identity *identity = nullptr;
	// Require a peer certificate. Syncthing peers are self-signed, so the chain isn't checked against CAs: the caller
	// checks peer_device_id() instead.
	bool require_peer_cert = true;
	// For HTTPS clients: verify the server against these PEM roots, and its name against server_name.
	std::string ca_pem;
	std::string server_name;
};

// One TLS connection over a Stream; blocking.
class TlsConnection {
public:
	~TlsConnection();
	TlsConnection(const TlsConnection &) = delete;
	TlsConnection &operator=(const TlsConnection &) = delete;

	// Runs the handshake; nullptr (and error) if it fails.
	static std::unique_ptr<TlsConnection> handshake(std::unique_ptr<Stream> stream, const TlsOptions &options,
		std::string *error = nullptr);

	long read(std::span<uint8_t> buffer);
	// Reads exactly buffer.size() bytes; false on error or end of stream.
	bool read_full(std::span<uint8_t> buffer);
	bool write_all(std::span<const uint8_t> data);
	void close();

	// The peer's leaf certificate (DER) and its device ID; empty if it sent none.
	const std::vector<uint8_t> &peer_cert() const { return peer_cert_; }
	DeviceId peer_device_id() const { return DeviceId::from_certificate(peer_cert_); }
	std::string alpn() const { return alpn_; }
	std::string version() const;
	std::string cipher() const;

private:
	TlsConnection() = default;
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::vector<uint8_t> peer_cert_;
	std::string alpn_;
};

// Initializes wolfSSL once per process; safe to call repeatedly.
void tls_init();

} // namespace xst
