#include "net/connection_service.hpp"

#include <chrono>
#include <condition_variable>
#include <map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/version.hpp"
#include "net/bep_connection.hpp"
#include "net/bep_handshake.hpp"
#include "net/tcp.hpp"
#include "platform/platform.hpp"

namespace xst {

namespace {

constexpr auto kPingInterval = std::chrono::seconds(90);
constexpr auto kRequestTimeout = std::chrono::seconds(60);

bool is_private_ipv4(const std::string &host)
{
	in_addr a{};
	if (inet_pton(AF_INET, host.c_str(), &a) != 1)
		return false;
	const uint32_t ip = ntohl(a.s_addr);
	return (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) || (ip >> 24) == 127 ||
		(ip >> 16) == (169 << 8 | 254);
}

// "tcp://host:port", "tcp4://...", "host:port" -> host, port. IPv6 addresses are skipped (the console has none).
bool parse_tcp(const std::string &uri, std::string &host, uint16_t &port)
{
	std::string rest = uri;
	for (const char *scheme : { "tcp://", "tcp4://" })
		if (rest.starts_with(scheme))
			rest = rest.substr(std::string(scheme).size());
	if (rest.find("://") != std::string::npos || rest.starts_with("["))
		return false;
	const auto q = rest.find('?');
	if (q != std::string::npos)
		rest = rest.substr(0, q);
	const auto slash = rest.find('/');
	if (slash != std::string::npos)
		rest = rest.substr(0, slash);
	const auto colon = rest.rfind(':');
	if (colon == std::string::npos) {
		host = rest;
		port = 22000;
	} else {
		host = rest.substr(0, colon);
		port = static_cast<uint16_t>(std::atoi(rest.c_str() + colon + 1));
	}
	if (host.empty() || host == "0.0.0.0")
		return false;
	return port != 0;
}

// A BEP connection: the reader thread routes Responses to waiting requests and everything else to the model.
class Peer : public PeerConnection, public std::enable_shared_from_this<Peer> {
public:
	Peer(std::unique_ptr<BepConnection> bep, std::string address, std::string type, std::string version)
		: bep_(std::move(bep)), address_(std::move(address)), type_(std::move(type)), version_(std::move(version)),
		  started_(std::chrono::system_clock::now()), crypto_("TLS1.3-" + bep_->tls().cipher())
	{
		device_ = bep_->peer();
	}

	DeviceId device() const override { return device_; }
	bool send(const bep::Message &m) override
	{
		if (closed_)
			return false;
		const bool ok = bep_->send(m);
		if (ok) {
			last_send_ = std::chrono::steady_clock::now();
			out_bytes_ += 64; // header estimate; the payload is counted below
			if (auto *r = std::get_if<bep::Response>(&m))
				out_bytes_ += static_cast<int64_t>(r->data.size());
		}
		return ok;
	}
	std::optional<bep::Bytes> request(const bep::Request &req) override
	{
		auto waiter = std::make_shared<Waiter>();
		{
			std::lock_guard lock(mu_);
			waiting_[req.id] = waiter;
		}
		if (!send(req)) {
			std::lock_guard lock(mu_);
			waiting_.erase(req.id);
			return std::nullopt;
		}
		std::unique_lock lock(waiter->mu);
		waiter->cv.wait_for(lock, kRequestTimeout, [&] { return waiter->done || closed_.load(); });
		{
			std::lock_guard l2(mu_);
			waiting_.erase(req.id);
		}
		if (!waiter->done || waiter->response.code != bep::ErrorCode::no_error)
			return std::nullopt;
		return std::move(waiter->response.data);
	}
	void close(const std::string &reason) override
	{
		if (closed_.exchange(true))
			return;
		bep_->close(reason);
		std::lock_guard lock(mu_);
		for (auto &[id, w] : waiting_) {
			std::lock_guard wl(w->mu);
			w->cv.notify_all();
		}
	}
	std::string address() const override { return address_; }
	std::string type() const override { return type_; }
	std::string client_version() const override { return version_; }
	std::string crypto() const override { return crypto_; }
	bool is_local() const override
	{
		const auto colon = address_.rfind(':');
		return is_private_ipv4(address_.substr(0, colon));
	}
	int64_t in_bytes() const override { return in_bytes_; }
	int64_t out_bytes() const override { return out_bytes_; }
	std::chrono::system_clock::time_point started() const override { return started_; }

	// The reader loop; returns why it ended.
	std::string run(Model &model)
	{
		std::thread pinger([this] {
			while (!closed_) {
				std::unique_lock lock(ping_mu_);
				ping_cv_.wait_for(lock, std::chrono::seconds(5), [&] { return closed_.load(); });
				if (closed_)
					break;
				if (std::chrono::steady_clock::now() - last_send_.load() > kPingInterval)
					send(bep::Ping{});
			}
		});
		std::string error;
		while (!closed_) {
			auto m = bep_->receive(&error);
			if (!m)
				break;
			in_bytes_ += 64;
			if (auto *r = std::get_if<bep::Response>(&*m)) {
				in_bytes_ += static_cast<int64_t>(r->data.size());
				std::shared_ptr<Waiter> w;
				{
					std::lock_guard lock(mu_);
					auto it = waiting_.find(r->id);
					if (it != waiting_.end())
						w = it->second;
				}
				if (w) {
					std::lock_guard wl(w->mu);
					w->response = std::move(*r);
					w->done = true;
					w->cv.notify_all();
				}
				continue;
			}
			if (std::holds_alternative<bep::Close>(*m)) {
				error = "closed by peer: " + std::get<bep::Close>(*m).reason;
				model.handle(device_, *m);
				break;
			}
			if (std::holds_alternative<bep::Request>(*m)) {
				// Serve requests off the reader thread so a slow disk doesn't stall responses to our own requests.
				auto self = shared_from_this();
				auto req = std::get<bep::Request>(*m);
				std::thread([self, &model, req] { model.handle(self->device_, req); }).detach();
				continue;
			}
			model.handle(device_, *m);
		}
		closed_ = true;
		ping_cv_.notify_all();
		pinger.join();
		{
			std::lock_guard lock(mu_);
			for (auto &[id, w] : waiting_) {
				std::lock_guard wl(w->mu);
				w->cv.notify_all();
			}
		}
		return error.empty() ? "connection closed" : error;
	}

private:
	struct Waiter {
		std::mutex mu;
		std::condition_variable cv;
		bool done = false;
		bep::Response response;
	};
	std::unique_ptr<BepConnection> bep_;
	DeviceId device_;
	std::string address_, type_, version_;
	std::chrono::system_clock::time_point started_;
	std::string crypto_;
	std::atomic<bool> closed_{ false };
	std::atomic<int64_t> in_bytes_{ 0 }, out_bytes_{ 0 };
	std::atomic<std::chrono::steady_clock::time_point> last_send_{ std::chrono::steady_clock::now() };
	std::mutex mu_;
	std::map<int32_t, std::shared_ptr<Waiter>> waiting_;
	std::mutex ping_mu_;
	std::condition_variable ping_cv_;
};

} // namespace

ConnectionService::ConnectionService(Model &model, Config &config, const Identity &identity, std::string device_name)
	: model_(model), config_(config), identity_(identity), device_name_(std::move(device_name))
{
}

ConnectionService::~ConnectionService() { stop(); }

void ConnectionService::start()
{
	listener_ = std::thread([this] { listen_loop(); });
	dialer_ = std::thread([this] { dial_loop(); });
}

void ConnectionService::stop()
{
	if (stopping_.exchange(true))
		return;
	{
		std::lock_guard lock(listener_mu_);
		if (listener_ptr_)
			listener_ptr_->shutdown();
	}
	dial_cv_.notify_all();
	{
		// Wake every connection, whatever stage it's at; their threads then end.
		std::lock_guard lock(active_mu_);
		for (auto *s : active_)
			s->interrupt();
	}
	if (listener_.joinable())
		listener_.join();
	if (dialer_.joinable())
		dialer_.join();
	for (auto &c : model_.connections())
		c->close("shutting down");
	std::vector<std::thread> threads;
	{
		std::lock_guard lock(threads_mu_);
		threads.swap(conn_threads_);
	}
	for (auto &t : threads)
		if (t.joinable())
			t.join();
}

nlohmann::json ConnectionService::listener_status() const
{
	nlohmann::json out = nlohmann::json::object();
	const std::string uri = "tcp://0.0.0.0:" + std::to_string(listen_port_.load());
	out[uri] = { { "error", listen_error_.empty() ? nlohmann::json(nullptr) : nlohmann::json(listen_error_) },
		{ "lanAddresses", nlohmann::json::array({ uri }) }, { "wanAddresses", nlohmann::json::array({ uri }) } };
	return out;
}

void ConnectionService::dial_now() { dial_cv_.notify_all(); }

void ConnectionService::listen_loop()
{
	uint16_t port = 22000;
	for (const auto &a : config_.options()["listenAddresses"]) {
		std::string s = a;
		if (s == "default" || s.starts_with("quic") || s.starts_with("relay") || s.starts_with("dynamic"))
			continue;
		for (const char *scheme : { "tcp://", "tcp4://", "tcp6://" })
			if (s.starts_with(scheme))
				s = s.substr(std::string(scheme).size());
		const auto colon = s.rfind(':');
		if (colon != std::string::npos) {
			const int p = std::atoi(s.c_str() + colon + 1);
			if (p > 0 && p < 65536)
				port = static_cast<uint16_t>(p);
		}
	}
	std::string error;
	std::unique_ptr<TcpListener> listener;
	for (int attempt = 0; attempt < 10 && !stopping_; attempt++) {
		listener = TcpListener::listen(port, &error);
		if (listener)
			break;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	if (!listener) {
		listen_error_ = error;
		platform::log("connections: " + error);
		return;
	}
	listen_port_ = listener->port();
	{
		std::lock_guard lock(listener_mu_);
		listener_ptr_ = listener.get();
	}
	platform::log("connections: listening on tcp port " + std::to_string(listen_port_.load()));
	while (!stopping_) {
		std::string peer;
		auto stream = listener->accept(&peer, 500);
		if (!stream)
			continue; // timed out: check stopping_ again
		std::lock_guard lock(threads_mu_);
		conn_threads_.emplace_back([this, s = std::move(stream), peer]() mutable {
			run_connection(std::move(s), peer, "tcp-server", TlsOptions::Role::server, std::nullopt);
		});
	}
	std::lock_guard lock(listener_mu_);
	listener_ptr_ = nullptr;
}

void ConnectionService::accept_stream(std::unique_ptr<Stream> stream, const std::string &address,
	const std::string &type, TlsOptions::Role role)
{
	std::lock_guard lock(threads_mu_);
	conn_threads_.emplace_back([this, s = std::move(stream), address, type, role]() mutable {
		run_connection(std::move(s), address, type, role, std::nullopt);
	});
}

void ConnectionService::run_connection(std::unique_ptr<Stream> stream, std::string address, std::string type,
	TlsOptions::Role role, std::optional<DeviceId> expected)
{
	// Registered until this returns, so stop() can interrupt it at any stage (handshake, Hello, or running).
	Stream *raw = stream.get();
	{
		std::lock_guard lock(active_mu_);
		if (stopping_)
			return;
		active_.insert(raw);
	}
	struct Unregister {
		ConnectionService *self;
		Stream *raw;
		~Unregister()
		{
			std::lock_guard lock(self->active_mu_);
			self->active_.erase(raw);
		}
	} unregister{ this, raw };
	std::string error;
	auto tls = TlsConnection::handshake(std::move(stream), bep_tls_options(identity_, role), &error);
	if (!tls) {
		platform::log("connections: " + address + ": " + error);
		return;
	}
	const DeviceId remote = tls->peer_device_id();
	if (expected && remote != *expected) {
		platform::log("connections: " + address + " presented " + remote.to_string() + ", not the device dialed");
		return;
	}
	if (remote == config_.my_id()) {
		return; // connected to ourselves
	}
	Hello ours;
	ours.device_name = device_name_;
	ours.client_name = std::string(client_name());
	ours.client_version = std::string(client_version());
	ours.num_connections = 1;
	ours.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch())
						 .count();
	const auto hello = exchange_hello(*tls, ours, &error);
	if (!hello) {
		platform::log("connections: " + address + ": " + error);
		return;
	}
	if (!model_.accept_device(remote, hello->device_name, address)) {
		platform::log("connections: rejecting unknown device " + remote.to_string() + " (" + hello->device_name + ")");
		return;
	}
	if (model_.is_connected(remote) && role == TlsOptions::Role::client) {
		return; // already connected the other way
	}
	if (stopping_)
		return;
	const nlohmann::json dcfg = config_.device(remote);
	bep::CompressionPolicy policy = bep::CompressionPolicy::metadata;
	const std::string comp = dcfg.is_null() ? "metadata" : dcfg.value("compression", "metadata");
	if (comp == "never")
		policy = bep::CompressionPolicy::never;
	else if (comp == "always")
		policy = bep::CompressionPolicy::always;

	auto bep = std::make_unique<BepConnection>(std::move(tls), policy);
	auto peer = std::make_shared<Peer>(std::move(bep), address, type,
		hello->client_name + " " + hello->client_version);
	platform::log("connections: connected to " + remote.short_string() + " (" + hello->device_name + ", " +
		hello->client_name + " " + hello->client_version + ") at " + address + " " + type);
	model_.connected(peer, hello->device_name, hello->client_name, hello->client_version);
	if (stopping_)
		peer->close("shutting down");
	const std::string why = peer->run(model_);
	platform::log("connections: " + remote.short_string() + " disconnected: " + why);
	model_.disconnected(remote, why);
}

bool ConnectionService::try_dial(const DeviceId &device, const std::string &address)
{
	if (address.starts_with("relay://")) {
		if (!relay_dialer_)
			return false;
		std::string error;
		auto stream = relay_dialer_(address, device, &error);
		if (!stream)
			return false;
		run_connection(std::move(stream), address, "relay-client", TlsOptions::Role::client, device);
		return true;
	}
	std::string host;
	uint16_t port;
	if (!parse_tcp(address, host, port))
		return false;
	std::string error;
	auto tcp = TcpStream::connect(host, port, &error);
	if (!tcp)
		return false;
	run_connection(std::move(tcp), host + ":" + std::to_string(port), "tcp-client", TlsOptions::Role::client, device);
	return true;
}

void ConnectionService::dial_loop()
{
	std::map<DeviceId, bool> dialing;
	std::mutex dialing_mu;
	while (!stopping_) {
		for (const auto &d : config_.devices()) {
			if (stopping_)
				break;
			const auto id = DeviceId::parse(d.value("deviceID", ""));
			if (!id || *id == config_.my_id() || d.value("paused", false) || model_.is_connected(*id))
				continue;
			{
				std::lock_guard lock(dialing_mu);
				if (dialing[*id])
					continue;
				dialing[*id] = true;
			}
			std::vector<std::string> addresses;
			for (const auto &a : d["addresses"]) {
				const std::string s = a;
				if (s == "dynamic") {
					for (auto *src : sources_)
						for (auto &found : src->lookup(*id))
							addresses.push_back(found);
				} else {
					addresses.push_back(s);
				}
			}
			// TCP first (LAN before WAN), relays last, as connection priorities order them upstream.
			std::stable_sort(addresses.begin(), addresses.end(), [](const std::string &a, const std::string &b) {
				auto rank = [](const std::string &s) {
					if (s.starts_with("relay://"))
						return 2;
					std::string host;
					uint16_t port;
					return parse_tcp(s, host, port) && is_private_ipv4(host) ? 0 : 1;
				};
				return rank(a) < rank(b);
			});
			std::lock_guard lock(threads_mu_);
			conn_threads_.emplace_back([this, dev = *id, addresses, &dialing, &dialing_mu] {
				for (const auto &a : addresses) {
					if (stopping_ || model_.is_connected(dev))
						break;
					if (try_dial(dev, a))
						break; // the connection ran (and has ended): next round decides again
				}
				std::lock_guard l(dialing_mu);
				dialing[dev] = false;
			});
		}
		std::unique_lock lock(dial_mu_);
		const int interval = config_.options().value("reconnectionIntervalS", 20);
		dial_cv_.wait_for(lock, std::chrono::seconds(std::max(5, interval)), [&] { return stopping_.load(); });
	}
	// Wait for the dial threads still holding references to dialing.
	std::vector<std::thread> threads;
	{
		std::lock_guard lock(threads_mu_);
		threads.swap(conn_threads_);
	}
	for (auto &d : model_.connections())
		d->close("shutting down");
	for (auto &t : threads)
		if (t.joinable())
			t.join();
}

} // namespace xst
