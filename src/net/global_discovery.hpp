#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "net/connection_service.hpp"
#include "net/identity.hpp"

namespace xst {

// Syncthing's global discovery client (lib/discover/global.go) over HTTPS/1.1:
//   announce: POST <server> {"addresses": [...]} with our device certificate as the TLS client certificate; the
//             server answers 204, maybe with Reannounce-After (seconds) or Retry-After on errors.
//   lookup:   GET <server>?device=<ID> -> {"addresses": [...]}; 404 if unknown; Retry-After on errors.
// Server URLs come from options.globalAnnounceServers ("default" expands to Syncthing's), with ?id=<device ID> (pin
// the server's certificate instead of CAs), ?insecure, ?noannounce and ?nolookup as upstream.
class GlobalDiscovery : public Service, public AddressSource {
public:
	using AddressesFn = std::function<std::vector<std::string>()>;
	GlobalDiscovery(Config &config, const Identity &identity, AddressesFn our_addresses);
	~GlobalDiscovery() override;

	void start() override;
	void stop() override;
	std::vector<std::string> lookup(const DeviceId &device) override;

	// server -> error ("" when fine), for /rest/system/status.
	std::map<std::string, std::string> errors() const;

	struct Server {
		std::string url; // without options
		std::string pinned_id;
		bool insecure = false, no_announce = false, no_lookup = false;
	};
	static std::vector<Server> servers_from(const nlohmann::json &options);

private:
	void announce_loop();
	bool announce_to(const Server &s, int *next_s);

	Config &config_;
	const Identity &identity_;
	AddressesFn our_addresses_;
	std::atomic<bool> stopping_{ false };
	std::thread thread_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::map<std::string, std::string> errors_;
	struct Cached {
		std::vector<std::string> addresses;
		std::chrono::steady_clock::time_point until;
	};
	std::map<DeviceId, Cached> cache_;
};

} // namespace xst
