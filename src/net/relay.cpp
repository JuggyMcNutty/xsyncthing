#include "net/relay.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include <arpa/inet.h>

#include "net/http_client.hpp"
#include "net/tcp.hpp"
#include "platform/platform.hpp"

namespace xst {

namespace relay {

namespace {

enum Type : uint32_t {
	kPing = 0,
	kPong = 1,
	kJoinRelayRequest = 2,
	kJoinSessionRequest = 3,
	kResponse = 4,
	kConnectRequest = 5,
	kSessionInvitation = 6,
	kRelayFull = 7,
};

struct Writer {
	std::vector<uint8_t> out;
	void u32(uint32_t v)
	{
		for (int s = 24; s >= 0; s -= 8)
			out.push_back(static_cast<uint8_t>(v >> s));
	}
	// XDR opaque/string: length, bytes, padded to 4.
	void bytes(std::span<const uint8_t> b)
	{
		u32(static_cast<uint32_t>(b.size()));
		out.insert(out.end(), b.begin(), b.end());
		while (out.size() % 4)
			out.push_back(0);
	}
	void str(const std::string &s) { bytes(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size())); }
};

struct Reader {
	std::span<const uint8_t> in;
	size_t pos = 0;
	bool ok = true;
	uint32_t u32()
	{
		if (in.size() - pos < 4) {
			ok = false;
			return 0;
		}
		const uint32_t v = static_cast<uint32_t>(in[pos]) << 24 | static_cast<uint32_t>(in[pos + 1]) << 16 |
			static_cast<uint32_t>(in[pos + 2]) << 8 | in[pos + 3];
		pos += 4;
		return v;
	}
	std::vector<uint8_t> bytes(size_t max)
	{
		const uint32_t n = u32();
		const size_t padded = (n + 3) & ~size_t(3);
		if (!ok || n > max || in.size() - pos < padded) {
			ok = false;
			return {};
		}
		std::vector<uint8_t> b(in.begin() + static_cast<long>(pos), in.begin() + static_cast<long>(pos + n));
		pos += padded;
		return b;
	}
	std::string str(size_t max)
	{
		const auto b = bytes(max);
		return std::string(b.begin(), b.end());
	}
};

} // namespace

std::vector<uint8_t> encode(const Message &m)
{
	Writer body;
	uint32_t type = 0;
	std::visit(
		[&](const auto &msg) {
			using T = std::decay_t<decltype(msg)>;
			if constexpr (std::is_same_v<T, Ping>)
				type = kPing;
			else if constexpr (std::is_same_v<T, Pong>)
				type = kPong;
			else if constexpr (std::is_same_v<T, RelayFull>)
				type = kRelayFull;
			else if constexpr (std::is_same_v<T, JoinRelayRequest>) {
				type = kJoinRelayRequest;
				body.str(msg.token);
			} else if constexpr (std::is_same_v<T, JoinSessionRequest>) {
				type = kJoinSessionRequest;
				body.bytes(msg.key);
			} else if constexpr (std::is_same_v<T, Response>) {
				type = kResponse;
				body.u32(static_cast<uint32_t>(msg.code));
				body.str(msg.message);
			} else if constexpr (std::is_same_v<T, ConnectRequest>) {
				type = kConnectRequest;
				body.bytes(msg.id);
			} else if constexpr (std::is_same_v<T, SessionInvitation>) {
				type = kSessionInvitation;
				body.bytes(msg.from);
				body.bytes(msg.key);
				body.bytes(msg.address);
				body.u32(msg.port); // XDR packs a uint16 into 4 bytes
				body.u32(msg.server_socket ? 1 : 0);
			}
		},
		m);
	Writer out;
	out.u32(kMagic);
	out.u32(type);
	out.u32(static_cast<uint32_t>(body.out.size()));
	out.out.insert(out.out.end(), body.out.begin(), body.out.end());
	return out.out;
}

std::optional<Message> decode(uint32_t type, std::span<const uint8_t> body)
{
	Reader r{ body };
	Message m;
	switch (type) {
	case kPing:
		m = Ping{};
		break;
	case kPong:
		m = Pong{};
		break;
	case kRelayFull:
		m = RelayFull{};
		break;
	case kJoinRelayRequest:
		m = JoinRelayRequest{ body.empty() ? "" : r.str(1024) };
		break;
	case kJoinSessionRequest:
		m = JoinSessionRequest{ r.bytes(32) };
		break;
	case kResponse: {
		Response resp;
		resp.code = static_cast<int32_t>(r.u32());
		resp.message = r.str(1024);
		m = resp;
		break;
	}
	case kConnectRequest:
		m = ConnectRequest{ r.bytes(32) };
		break;
	case kSessionInvitation: {
		SessionInvitation inv;
		inv.from = r.bytes(32);
		inv.key = r.bytes(32);
		inv.address = r.bytes(32);
		inv.port = static_cast<uint16_t>(r.u32());
		inv.server_socket = r.u32() != 0;
		m = inv;
		break;
	}
	default:
		return std::nullopt;
	}
	if (!r.ok)
		return std::nullopt;
	return m;
}

} // namespace relay

namespace {

constexpr int kConnectTimeoutMs = 10000;
constexpr int kMessageTimeoutMs = 2 * 60 * 1000;

// Reads/writes relay messages over a TLS connection or a plain stream.
struct Channel {
	TlsConnection *tls = nullptr;
	Stream *plain = nullptr;
	bool write(const relay::Message &m)
	{
		const auto b = relay::encode(m);
		if (tls)
			return tls->write_all(b);
		return plain->write(b) == static_cast<long>(b.size());
	}
	bool read_full(std::span<uint8_t> b)
	{
		if (tls)
			return tls->read_full(b);
		size_t done = 0;
		while (done < b.size()) {
			const long n = plain->read(b.subspan(done));
			if (n <= 0)
				return false;
			done += static_cast<size_t>(n);
		}
		return true;
	}
	std::optional<relay::Message> read()
	{
		uint8_t h[12];
		if (!read_full(h))
			return std::nullopt;
		auto u32 = [&](int i) {
			return static_cast<uint32_t>(h[i]) << 24 | static_cast<uint32_t>(h[i + 1]) << 16 |
				static_cast<uint32_t>(h[i + 2]) << 8 | h[i + 3];
		};
		if (u32(0) != relay::kMagic)
			return std::nullopt;
		const uint32_t len = u32(8);
		if (len > 1024)
			return std::nullopt;
		std::vector<uint8_t> body(len);
		if (len && !read_full(body))
			return std::nullopt;
		return relay::decode(u32(4), body);
	}
};

struct RelayUri {
	std::string host;
	uint16_t port = 0;
	std::string id, token;
};

std::optional<RelayUri> parse_relay(const std::string &uri)
{
	if (!uri.starts_with("relay://"))
		return std::nullopt;
	const auto u = parse_url("https://" + uri.substr(8));
	if (!u)
		return std::nullopt;
	RelayUri r;
	r.host = u->host;
	r.port = u->port == 443 && uri.find(":443") == std::string::npos ? 22067 : u->port;
	r.id = u->params.count("id") ? u->params.at("id") : "";
	r.token = u->params.count("token") ? u->params.at("token") : "";
	return r;
}

// TLS to the relay's control port: ALPN bep-relay, both certificates, the relay's matching ?id= if given.
std::unique_ptr<TlsConnection> control_connect(const RelayUri &r, const Identity &identity, Stream **raw,
	std::string *error)
{
	auto tcp = TcpStream::connect(r.host, r.port, error, kConnectTimeoutMs);
	if (!tcp)
		return nullptr;
	tcp->set_timeout(kMessageTimeoutMs);
	if (raw)
		*raw = tcp.get();
	TlsOptions o;
	o.role = TlsOptions::Role::client;
	o.min_version = 12;
	o.alpn = { "bep-relay" };
	o.identity = &identity;
	o.require_peer_cert = true;
	auto tls = TlsConnection::handshake(std::move(tcp), o, error);
	if (!tls)
		return nullptr;
	if (tls->alpn() != "bep-relay") {
		if (error)
			*error = "protocol negotiation error";
		return nullptr;
	}
	if (!r.id.empty() && DeviceId::parse(r.id) != tls->peer_device_id()) {
		if (error)
			*error = "relay id does not match";
		return nullptr;
	}
	return tls;
}

std::string ip_text(const std::vector<uint8_t> &address, const std::string &fallback)
{
	if (address.size() == 4 && !(address[0] == 0 && address[1] == 0 && address[2] == 0 && address[3] == 0)) {
		char t[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, address.data(), t, sizeof t);
		return t;
	}
	return fallback; // unspecified: the relay's own address
}

// Session connection: plain TCP to the invitation's address, JoinSessionRequest, expect Response 0.
std::unique_ptr<Stream> join_session(const relay::SessionInvitation &inv, const std::string &relay_host,
	std::string *error)
{
	auto tcp = TcpStream::connect(ip_text(inv.address, relay_host), inv.port, error, kConnectTimeoutMs);
	if (!tcp)
		return nullptr;
	tcp->set_timeout(kConnectTimeoutMs);
	Channel ch{ nullptr, tcp.get() };
	if (!ch.write(relay::JoinSessionRequest{ inv.key })) {
		if (error)
			*error = "writing JoinSessionRequest failed";
		return nullptr;
	}
	const auto m = ch.read();
	const auto *resp = m ? std::get_if<relay::Response>(&*m) : nullptr;
	if (!resp || resp->code != 0) {
		if (error)
			*error = resp ? "session refused: " + resp->message : "no response to JoinSessionRequest";
		return nullptr;
	}
	tcp->set_timeout(0);
	return tcp;
}

} // namespace

RelayService::RelayService(Config &config, const Identity &identity, ConnectionService &connections,
	std::function<void(std::vector<std::string>)> set_announced)
	: config_(config), identity_(identity), connections_(connections), set_announced_(std::move(set_announced))
{
}

RelayService::~RelayService() { stop(); }

void RelayService::start()
{
	if (!config_.options().value("relaysEnabled", true))
		return;
	thread_ = std::thread([this] { run(); });
}

void RelayService::stop()
{
	stopping_ = true;
	{
		std::lock_guard lock(mu_);
		if (current_)
			current_->interrupt();
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

std::string RelayService::joined_uri() const
{
	std::lock_guard lock(mu_);
	return joined_;
}

std::string RelayService::error() const
{
	std::lock_guard lock(mu_);
	return error_;
}

std::vector<std::string> RelayService::candidates()
{
	std::vector<std::string> out;
	const nlohmann::json listen = config_.options().value("listenAddresses", nlohmann::json::array({ "default" }));
	for (const auto &a : listen) {
		const std::string s = a;
		if (s.starts_with("relay://")) {
			out.push_back(s);
		} else if (s == "default" || s.starts_with("dynamic+")) {
			const std::string pool =
				s == "default" ? "https://relays.syncthing.net/endpoint" : s.substr(std::string("dynamic+").size());
			HttpRequest req;
			req.url = pool;
			req.ca_pem = ca_bundle();
			std::string error;
			const auto resp = http_request(req, &error);
			if (!resp || resp->status != 200) {
				std::lock_guard lock(mu_);
				error_ = "relay pool: " + (resp ? std::to_string(resp->status) : error);
				continue;
			}
			try {
				std::vector<std::string> pool_relays;
				const nlohmann::json pool_json = nlohmann::json::parse(resp->body).value("relays", nlohmann::json::array());
				for (const auto &r : pool_json)
					pool_relays.push_back(r.value("url", ""));
				// Upstream orders by measured latency; we try a few at random, which spreads load the same way.
				std::shuffle(pool_relays.begin(), pool_relays.end(), std::mt19937(std::random_device{}()));
				for (const auto &r : pool_relays)
					if (!r.empty())
						out.push_back(r);
			} catch (...) {
			}
		}
	}
	return out;
}

bool RelayService::serve_relay(const std::string &uri)
{
	const auto r = parse_relay(uri);
	if (!r)
		return false;
	std::string error;
	Stream *raw = nullptr;
	auto tls = control_connect(*r, identity_, &raw, &error);
	if (!tls) {
		std::lock_guard lock(mu_);
		error_ = uri + ": " + error;
		return false;
	}
	{
		std::lock_guard lock(mu_);
		current_ = raw;
	}
	struct Clear {
		RelayService *s;
		~Clear()
		{
			std::lock_guard lock(s->mu_);
			s->current_ = nullptr;
			s->joined_.clear();
		}
	} clear{ this };
	Channel ch{ tls.get(), nullptr };
	if (!ch.write(relay::JoinRelayRequest{ r->token })) {
		return false;
	}
	const auto joined = ch.read();
	const auto *resp = joined ? std::get_if<relay::Response>(&*joined) : nullptr;
	if (!resp || resp->code != 0) {
		std::lock_guard lock(mu_);
		error_ = uri + ": " + (resp ? resp->message : std::string(joined ? "relay full" : "no response"));
		return false;
	}
	// Announce the relay as one of our addresses, without the token.
	std::string announced = "relay://" + r->host + ":" + std::to_string(r->port) + "/";
	if (!r->id.empty())
		announced += "?id=" + r->id;
	{
		std::lock_guard lock(mu_);
		joined_ = announced;
		error_.clear();
	}
	set_announced_({ announced });
	platform::log("relay: joined " + announced);
	while (!stopping_) {
		const auto m = ch.read();
		if (!m)
			break;
		if (std::holds_alternative<relay::Ping>(*m)) {
			if (!ch.write(relay::Pong{}))
				break;
		} else if (const auto *inv = std::get_if<relay::SessionInvitation>(&*m)) {
			const auto from = DeviceId::from_bytes(inv->from);
			std::string e;
			auto session = join_session(*inv, r->host, &e);
			if (!session) {
				platform::log("relay: session from " + (from ? from->short_string() : "?") + " failed: " + e);
				continue;
			}
			connections_.accept_stream(std::move(session), r->host + ":" + std::to_string(inv->port),
				"relay-server", inv->server_socket ? TlsOptions::Role::server : TlsOptions::Role::client);
		} else if (std::holds_alternative<relay::RelayFull>(*m)) {
			std::lock_guard lock(mu_);
			error_ = uri + ": relay full";
			break;
		} else {
			break; // protocol error
		}
	}
	set_announced_({});
	return true;
}

void RelayService::run()
{
	while (!stopping_) {
		bool served = false;
		for (const auto &c : candidates()) {
			if (stopping_)
				break;
			if (serve_relay(c)) {
				served = true;
				break; // came back: pick again
			}
		}
		// Upstream reconnects after relayReconnectIntervalM; a lost relay is replaced sooner.
		const int minutes = config_.options().value("relayReconnectIntervalM", 10);
		std::unique_lock lock(mu_);
		cv_.wait_for(lock, served ? std::chrono::seconds(10) : std::chrono::minutes(std::max(1, minutes)),
			[&] { return stopping_.load(); });
	}
}

std::unique_ptr<Stream> RelayService::dial(const std::string &uri, const DeviceId &device, const Identity &identity,
	bool *server, std::string *error)
{
	const auto r = parse_relay(uri);
	if (!r) {
		if (error)
			*error = "not a relay URI";
		return nullptr;
	}
	auto tls = control_connect(*r, identity, nullptr, error);
	if (!tls)
		return nullptr;
	Channel ch{ tls.get(), nullptr };
	if (!ch.write(relay::ConnectRequest{ std::vector<uint8_t>(device.bytes().begin(), device.bytes().end()) }))
		return nullptr;
	const auto m = ch.read();
	tls->close();
	if (!m) {
		if (error)
			*error = "no answer to ConnectRequest";
		return nullptr;
	}
	if (const auto *resp = std::get_if<relay::Response>(&*m)) {
		if (error)
			*error = "relay: " + resp->message;
		return nullptr;
	}
	const auto *inv = std::get_if<relay::SessionInvitation>(&*m);
	if (!inv)
		return nullptr;
	*server = inv->server_socket;
	return join_session(*inv, r->host, error);
}

} // namespace xst
