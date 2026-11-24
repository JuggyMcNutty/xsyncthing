#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/model.hpp"
#include "net/identity.hpp"
#include "net/tls.hpp"

namespace xst {

// Addresses where a device may be reached, by its ID (local discovery, global discovery, relays).
class AddressSource {
public:
	virtual ~AddressSource() = default;
	virtual std::vector<std::string> lookup(const DeviceId &device) = 0;
};

// Connects a stream to a relay session for a device (net/relay); nullptr if relays can't reach it.
using RelayDialer = std::function<std::unique_ptr<Stream>(const std::string &relay_uri, const DeviceId &device,
	std::string *error)>;

// Listens for BEP connections, dials configured devices, and runs each connection: TLS 1.3, Hello, then BEP
// messages to and from the model. As lib/connections.
class ConnectionService {
public:
	ConnectionService(Model &model, Config &config, const Identity &identity, std::string device_name);
	~ConnectionService();

	void start();
	void stop();
	void add_address_source(AddressSource *source) { sources_.push_back(source); }
	void set_relay_dialer(RelayDialer d) { relay_dialer_ = std::move(d); }
	// Hands an already-established stream (a relay session we were invited to) over as a server-side connection.
	void accept_stream(std::unique_ptr<Stream> stream, const std::string &address, const std::string &type,
		TlsOptions::Role role);

	uint16_t listen_port() const { return listen_port_; }
	// Status for /rest/system/status.connectionServiceStatus.
	nlohmann::json listener_status() const;
	// Dial now instead of waiting for the next round (after a config change).
	void dial_now();

private:
	void listen_loop();
	void dial_loop();
	void run_connection(std::unique_ptr<Stream> stream, std::string address, std::string type, TlsOptions::Role role,
		std::optional<DeviceId> expected);
	bool try_dial(const DeviceId &device, const std::string &address);

	Model &model_;
	Config &config_;
	const Identity &identity_;
	std::string device_name_;
	std::vector<AddressSource *> sources_;
	RelayDialer relay_dialer_;
	std::atomic<bool> stopping_{ false };
	std::atomic<uint16_t> listen_port_{ 0 };
	std::string listen_error_;
	std::thread listener_, dialer_;
	std::mutex threads_mu_;
	std::vector<std::thread> conn_threads_;
	std::mutex dial_mu_;
	std::condition_variable dial_cv_;
	std::mutex listener_mu_;
	class TcpListener *listener_ptr_ = nullptr;
};

} // namespace xst
