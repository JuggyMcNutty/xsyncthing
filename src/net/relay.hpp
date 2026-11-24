#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "app.hpp"
#include "net/connection_service.hpp"
#include "net/identity.hpp"

namespace xst {

// Syncthing's relay protocol (lib/relay/protocol): XDR messages, each a header (magic 0x9E79BC40, type, length, as
// big-endian u32s) and a body of at most 1024 bytes. The control connection is TLS (ALPN "bep-relay", both sides'
// certificates, the relay's checked against the ?id= in its URI); the session connection is plain TCP, carrying the
// two devices' BEP TLS straight through once joined.
namespace relay {

inline constexpr uint32_t kMagic = 0x9E79BC40;

struct Ping {};
struct Pong {};
struct JoinRelayRequest {
	std::string token;
};
struct JoinSessionRequest {
	std::vector<uint8_t> key;
};
struct Response {
	int32_t code = 0;
	std::string message;
};
struct ConnectRequest {
	std::vector<uint8_t> id;
};
struct SessionInvitation {
	std::vector<uint8_t> from, key, address;
	uint16_t port = 0;
	bool server_socket = false;
};
struct RelayFull {};

using Message = std::variant<Ping, Pong, JoinRelayRequest, JoinSessionRequest, Response, ConnectRequest,
	SessionInvitation, RelayFull>;

std::vector<uint8_t> encode(const Message &m);
// Decodes one message from its header and body; nullopt if malformed.
std::optional<Message> decode(uint32_t type, std::span<const uint8_t> body);

} // namespace relay

// The relay service: joins a relay (from options.listenAddresses' "dynamic+https://relays.syncthing.net/endpoint" or a
// static relay:// URI), announces it, and hands each session invitation to the connection service; and dials devices
// through their relays (relay:// addresses from discovery).
class RelayService : public Service {
public:
	RelayService(Config &config, const Identity &identity, ConnectionService &connections,
		std::function<void(std::vector<std::string>)> set_announced);
	~RelayService() override;

	void start() override;
	void stop() override;

	// Our relay address while joined ("" otherwise), and the last error.
	std::string joined_uri() const;
	std::string error() const;

	// Dials device through the relay at uri: ConnectRequest for an invitation, then JoinSession. The stream that
	// comes back carries the BEP TLS; *server is true if we must be the TLS server on it.
	static std::unique_ptr<Stream> dial(const std::string &uri, const DeviceId &device, const Identity &identity,
		bool *server, std::string *error);

private:
	void run();
	bool serve_relay(const std::string &uri);
	std::vector<std::string> candidates();

	Config &config_;
	const Identity &identity_;
	ConnectionService &connections_;
	std::function<void(std::vector<std::string>)> set_announced_;
	std::atomic<bool> stopping_{ false };
	std::thread thread_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::string joined_, error_;
	Stream *current_ = nullptr;
};

} // namespace xst
