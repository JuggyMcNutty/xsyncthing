#include "net/local_discovery.hpp"

#include <cerrno>
#include <cstring>
#include <random>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if !defined(__XSDK__)
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "core/protowire.hpp"
#include "platform/platform.hpp"

namespace xst {

namespace {
constexpr uint16_t kPort = 21027;
constexpr auto kInterval = std::chrono::seconds(30);
constexpr auto kLifetime = std::chrono::seconds(90);
} // namespace

std::vector<uint8_t> LocalAnnounce::encode() const
{
	protowire::Writer w;
	w.bytes(1, id.bytes());
	for (const auto &a : addresses)
		w.string(2, a);
	w.varint_nz(3, static_cast<uint64_t>(instance_id));
	std::vector<uint8_t> out = { static_cast<uint8_t>(kMagic >> 24), static_cast<uint8_t>(kMagic >> 16),
		static_cast<uint8_t>(kMagic >> 8), static_cast<uint8_t>(kMagic) };
	out.insert(out.end(), w.data().begin(), w.data().end());
	return out;
}

std::optional<LocalAnnounce> LocalAnnounce::decode(std::span<const uint8_t> packet)
{
	if (packet.size() < 4)
		return std::nullopt;
	const uint32_t magic = static_cast<uint32_t>(packet[0]) << 24 | static_cast<uint32_t>(packet[1]) << 16 |
		static_cast<uint32_t>(packet[2]) << 8 | packet[3];
	if (magic != kMagic)
		return std::nullopt;
	LocalAnnounce a;
	protowire::Reader r(packet.subspan(4));
	bool have_id = false;
	while (auto f = r.next()) {
		if (f->number == 1 && f->type == protowire::kLen) {
			if (auto id = DeviceId::from_bytes(f->bytes)) {
				a.id = *id;
				have_id = true;
			}
		} else if (f->number == 2 && f->type == protowire::kLen) {
			a.addresses.emplace_back(f->str());
		} else if (f->number == 3 && f->type == protowire::kVarint) {
			a.instance_id = static_cast<int64_t>(f->varint);
		}
	}
	if (r.error() || !have_id)
		return std::nullopt;
	return a;
}

std::vector<std::string> LocalAnnounce::resolved(const std::string &sender_ip) const
{
	std::vector<std::string> out;
	for (const auto &addr : addresses) {
		const auto scheme_end = addr.find("://");
		if (scheme_end == std::string::npos)
			continue;
		const std::string scheme = addr.substr(0, scheme_end);
		if (scheme == "tcp6" || scheme.starts_with("quic"))
			continue; // the console speaks TCP over IPv4 only
		std::string rest = addr.substr(scheme_end + 3);
		std::string query;
		if (const auto q = rest.find_first_of("/?"); q != std::string::npos) {
			query = rest.substr(q);
			rest = rest.substr(0, q);
		}
		if (rest.starts_with("[")) {
			// IPv6 literal: only an unspecified one ("[::]:port") is usable, as the sender's IPv4 address.
			const auto close = rest.find(']');
			if (close == std::string::npos || rest.substr(1, close - 1) != "::")
				continue;
			const std::string port = rest.substr(close + 1);
			out.push_back(scheme + "://" + sender_ip + port + query);
			continue;
		}
		const auto colon = rest.rfind(':');
		const std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
		const std::string port = colon == std::string::npos ? "" : rest.substr(colon);
		if (host.empty() || host == "0.0.0.0")
			out.push_back(scheme + "://" + sender_ip + port + query);
		else
			out.push_back(addr);
	}
	return out;
}

LocalDiscovery::LocalDiscovery(Config &config, Events &events, DeviceId my_id, std::function<uint16_t()> listen_port)
	: config_(config), events_(events), my_id_(my_id), listen_port_(std::move(listen_port))
{
	std::random_device rd;
	instance_id_ = static_cast<int64_t>((static_cast<uint64_t>(rd()) << 32 | rd()) & 0x7FFFFFFFFFFFFFFFULL);
}

LocalDiscovery::~LocalDiscovery() { stop(); }

void LocalDiscovery::start()
{
	if (!config_.options().value("localAnnounceEnabled", true))
		return;
	thread_ = std::thread([this] { run(); });
}

void LocalDiscovery::stop()
{
	stopping_ = true;
	if (thread_.joinable())
		thread_.join();
}

std::string LocalDiscovery::error() const
{
	std::lock_guard lock(mu_);
	return error_;
}

std::vector<std::string> LocalDiscovery::lookup(const DeviceId &device)
{
	std::lock_guard lock(mu_);
	auto it = cache_.find(device);
	if (it == cache_.end() || std::chrono::steady_clock::now() - it->second.when > kLifetime)
		return {};
	return it->second.addresses;
}

std::map<DeviceId, std::vector<std::string>> LocalDiscovery::cache() const
{
	std::lock_guard lock(mu_);
	std::map<DeviceId, std::vector<std::string>> out;
	const auto now = std::chrono::steady_clock::now();
	for (const auto &[id, e] : cache_)
		if (now - e.when <= kLifetime)
			out[id] = e.addresses;
	return out;
}

void LocalDiscovery::send_announcement(int fd)
{
	const uint16_t port = listen_port_();
	if (port == 0)
		return;
	LocalAnnounce a;
	a.id = my_id_;
	a.addresses = { "tcp://0.0.0.0:" + std::to_string(port) };
	a.instance_id = instance_id_;
	const auto packet = a.encode();

	std::vector<in_addr> targets;
	in_addr all{};
	all.s_addr = htonl(INADDR_BROADCAST);
	targets.push_back(all);
#if !defined(__XSDK__)
	// On the host, also each interface's own broadcast address, as lib/beacon does.
	ifaddrs *ifs = nullptr;
	if (getifaddrs(&ifs) == 0) {
		for (auto *i = ifs; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) &&
				(i->ifa_flags & IFF_RUNNING) && i->ifa_broadaddr)
				targets.push_back(reinterpret_cast<sockaddr_in *>(i->ifa_broadaddr)->sin_addr);
		freeifaddrs(ifs);
	}
#endif
	int sent = 0;
	for (const auto &t : targets) {
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(kPort);
		to.sin_addr = t;
		if (::sendto(fd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof to) ==
			static_cast<ssize_t>(packet.size()))
			sent++;
	}
	std::lock_guard lock(mu_);
	error_ = sent ? "" : std::string("sending the announcement failed: ") + std::strerror(errno);
}

void LocalDiscovery::run()
{
	const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		std::lock_guard lock(mu_);
		error_ = std::string("socket: ") + std::strerror(errno);
		platform::log("discovery: " + error_);
		return;
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#if defined(SO_REUSEPORT)
	::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
		std::lock_guard lock(mu_);
		error_ = std::string("bind to UDP 21027: ") + std::strerror(errno);
		platform::log("discovery: " + error_ + " (announcing only)");
	} else {
		platform::log("discovery: local discovery on UDP 21027 (IPv4 broadcast)");
	}

	auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2); // once the listener has a port
	std::vector<uint8_t> buf(2048);
	while (!stopping_) {
		if (std::chrono::steady_clock::now() >= next) {
			send_announcement(fd);
			next = std::chrono::steady_clock::now() + kInterval;
		}
		pollfd p{ fd, POLLIN, 0 };
		if (::poll(&p, 1, 500) <= 0 || !(p.revents & POLLIN))
			continue;
		sockaddr_in from{};
		socklen_t len = sizeof from;
		const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &len);
		if (n <= 0)
			continue;
		const auto a = LocalAnnounce::decode(std::span(buf.data(), static_cast<size_t>(n)));
		if (!a || a->id == my_id_)
			continue;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
		const auto addresses = a->resolved(ip);
		bool is_new;
		{
			std::lock_guard lock(mu_);
			auto it = cache_.find(a->id);
			is_new = it == cache_.end() || it->second.instance_id != a->instance_id ||
				std::chrono::steady_clock::now() - it->second.when > kLifetime;
			cache_[a->id] = { addresses, std::chrono::steady_clock::now(), a->instance_id };
		}
		if (is_new) {
			std::string list;
			for (const auto &s : addresses)
				list += (list.empty() ? "" : ", ") + s;
			platform::log("discovery: " + a->id.short_string() + " at " + list);
			nlohmann::json addrs = addresses;
			events_.log("DeviceDiscovered", { { "device", a->id.to_string() }, { "addrs", addrs } });
			next = std::chrono::steady_clock::now(); // answer a new device right away, as upstream does
		}
	}
	::close(fd);
}

} // namespace xst
