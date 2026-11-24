#include "net/global_discovery.hpp"

#include "net/http_client.hpp"
#include "platform/platform.hpp"

namespace xst {

namespace {

constexpr int kReannounceS = 30 * 60;
constexpr int kErrorRetryS = 5 * 60;
constexpr auto kPositiveCache = std::chrono::minutes(5);
constexpr auto kNegativeCache = std::chrono::minutes(1);

// Syncthing's defaults (lib/config DefaultDiscoveryServersV4). IPv6 announce servers are left out: no IPv6 here.
const std::vector<std::string> kDefaultServers = {
	"https://discovery-lookup.syncthing.net/v2/?noannounce",
	"https://discovery-announce-v4.syncthing.net/v2/?nolookup",
};

bool query_bool(const std::map<std::string, std::string> &p, const std::string &k)
{
	auto it = p.find(k);
	return it != p.end() && it->second != "false";
}

} // namespace

std::vector<GlobalDiscovery::Server> GlobalDiscovery::servers_from(const nlohmann::json &options)
{
	std::vector<std::string> raw;
	for (const auto &s : options.value("globalAnnounceServers", nlohmann::json::array({ "default" }))) {
		const std::string v = s;
		if (v == "default" || v == "default-v4")
			raw.insert(raw.end(), kDefaultServers.begin(), kDefaultServers.end());
		else if (v != "default-v6")
			raw.push_back(v);
	}
	std::vector<Server> out;
	for (const auto &r : raw) {
		const auto u = parse_url(r);
		if (!u)
			continue;
		Server s;
		s.pinned_id = u->params.count("id") ? u->params.at("id") : "";
		s.insecure = !s.pinned_id.empty() || query_bool(u->params, "insecure");
		s.no_announce = query_bool(u->params, "noannounce");
		s.no_lookup = query_bool(u->params, "nolookup");
		if (u->scheme == "http" && !(s.insecure && s.no_announce))
			continue; // as upstream: plain HTTP only for insecure lookups
		const auto q = r.find('?');
		s.url = q == std::string::npos ? r : r.substr(0, q);
		out.push_back(s);
	}
	return out;
}

GlobalDiscovery::GlobalDiscovery(Config &config, const Identity &identity, AddressesFn our_addresses)
	: config_(config), identity_(identity), our_addresses_(std::move(our_addresses))
{
}

GlobalDiscovery::~GlobalDiscovery() { stop(); }

void GlobalDiscovery::start()
{
	if (!config_.options().value("globalAnnounceEnabled", true))
		return;
	thread_ = std::thread([this] { announce_loop(); });
}

void GlobalDiscovery::stop()
{
	stopping_ = true;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

std::map<std::string, std::string> GlobalDiscovery::errors() const
{
	std::lock_guard lock(mu_);
	return errors_;
}

bool GlobalDiscovery::announce_to(const Server &s, int *next_s)
{
	const auto addresses = our_addresses_();
	if (addresses.empty()) {
		*next_s = kErrorRetryS;
		return true; // nothing to announce yet
	}
	HttpRequest req;
	req.method = "POST";
	req.url = s.url;
	req.headers["Content-Type"] = "application/json";
	req.body = nlohmann::json{ { "addresses", addresses } }.dump();
	req.client_identity = &identity_;
	if (!s.insecure)
		req.ca_pem = ca_bundle();
	std::string error;
	const auto resp = http_request(req, &error);
	std::string err;
	if (!resp) {
		err = error;
		*next_s = kErrorRetryS;
	} else if (!s.pinned_id.empty() && DeviceId::from_certificate(resp->peer_cert) != DeviceId::parse(s.pinned_id)) {
		err = "security: incorrect device id";
		*next_s = kErrorRetryS;
	} else if (resp->status < 200 || resp->status > 299) {
		err = std::to_string(resp->status) + " " + resp->reason;
		const int retry = std::atoi(resp->header("retry-after").c_str());
		*next_s = retry > 0 ? retry : kErrorRetryS;
	} else {
		const int re = std::atoi(resp->header("reannounce-after").c_str());
		*next_s = re > 0 ? re : kReannounceS;
	}
	{
		std::lock_guard lock(mu_);
		const bool changed = errors_[s.url] != err;
		errors_[s.url] = err;
		if (changed)
			platform::log("discovery: announce to " + s.url + (err.empty() ? ": ok" : ": " + err));
	}
	return err.empty();
}

void GlobalDiscovery::announce_loop()
{
	std::map<std::string, std::chrono::steady_clock::time_point> next;
	// Give the listener a moment to come up before the first announcement.
	{
		std::unique_lock lock(mu_);
		cv_.wait_for(lock, std::chrono::seconds(3), [&] { return stopping_.load(); });
	}
	while (!stopping_) {
		const auto now = std::chrono::steady_clock::now();
		auto soonest = now + std::chrono::minutes(5);
		for (const auto &s : servers_from(config_.options())) {
			if (s.no_announce)
				continue;
			auto &n = next[s.url];
			if (n <= now) {
				int secs = kErrorRetryS;
				announce_to(s, &secs);
				n = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
			}
			soonest = std::min(soonest, n);
		}
		std::unique_lock lock(mu_);
		cv_.wait_until(lock, soonest, [&] { return stopping_.load(); });
	}
}

std::vector<std::string> GlobalDiscovery::lookup(const DeviceId &device)
{
	if (!config_.options().value("globalAnnounceEnabled", true))
		return {};
	{
		std::lock_guard lock(mu_);
		auto it = cache_.find(device);
		if (it != cache_.end() && std::chrono::steady_clock::now() < it->second.until)
			return it->second.addresses;
	}
	std::vector<std::string> found;
	auto cache_for = kNegativeCache;
	for (const auto &s : servers_from(config_.options())) {
		if (s.no_lookup || stopping_)
			continue;
		HttpRequest req;
		req.url = s.url + (s.url.find('?') == std::string::npos ? "?" : "&") + "device=" + device.to_string();
		if (!s.insecure)
			req.ca_pem = ca_bundle();
		req.timeout_ms = 15000;
		std::string error;
		const auto resp = http_request(req, &error);
		if (!resp || resp->status != 200)
			continue;
		if (!s.pinned_id.empty() && DeviceId::from_certificate(resp->peer_cert) != DeviceId::parse(s.pinned_id))
			continue;
		try {
			const auto j = nlohmann::json::parse(resp->body);
			for (const auto &a : j.value("addresses", nlohmann::json::array()))
				found.push_back(a.get<std::string>());
			cache_for = kPositiveCache;
		} catch (...) {
		}
		if (!found.empty())
			break;
	}
	std::lock_guard lock(mu_);
	cache_[device] = { found, std::chrono::steady_clock::now() + cache_for };
	return found;
}

} // namespace xst
