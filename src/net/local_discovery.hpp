#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "core/device_id.hpp"
#include "core/events.hpp"
#include "net/connection_service.hpp"

namespace xst {

// Syncthing's local discovery (lib/discover/local.go) over IPv4 broadcast only, as the console has no IPv6:
// every 30 s (and when a new device is heard), a UDP datagram to 255.255.255.255:21027 holding the magic 0x2EA7D90B
// and a protobuf Announce { id, addresses, instance_id }. Unspecified addresses ("tcp://0.0.0.0:22000") are taken to
// mean the sender's own address. Entries are kept for 90 s.
struct LocalAnnounce {
	static constexpr uint32_t kMagic = 0x2EA7D90B;
	static constexpr uint32_t kV13Magic = 0x7D79BC40;

	DeviceId id;
	std::vector<std::string> addresses;
	int64_t instance_id = 0;

	std::vector<uint8_t> encode() const;
	// nullopt if not a current-version announcement.
	static std::optional<LocalAnnounce> decode(std::span<const uint8_t> packet);
	// Addresses with an unspecified or missing host replaced by the sender's IPv4 address; others kept as sent.
	std::vector<std::string> resolved(const std::string &sender_ip) const;
};

class LocalDiscovery : public Service, public AddressSource {
public:
	LocalDiscovery(Config &config, Events &events, DeviceId my_id, std::function<uint16_t()> listen_port);
	~LocalDiscovery() override;

	void start() override;
	void stop() override;
	std::vector<std::string> lookup(const DeviceId &device) override;
	// For /rest/system/discovery: device -> addresses.
	std::map<DeviceId, std::vector<std::string>> cache() const;
	std::string error() const;

private:
	void run();
	void send_announcement(int fd);

	Config &config_;
	Events &events_;
	DeviceId my_id_;
	std::function<uint16_t()> listen_port_;
	int64_t instance_id_;
	std::atomic<bool> stopping_{ false };
	std::thread thread_;
	struct Entry {
		std::vector<std::string> addresses;
		std::chrono::steady_clock::time_point when;
		int64_t instance_id = 0;
	};
	mutable std::mutex mu_;
	std::map<DeviceId, Entry> cache_;
	std::string error_;
};

} // namespace xst
