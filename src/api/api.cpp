#include "api/api.hpp"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/stat.h>

#include "core/bcrypt.hpp"
#include "core/version.hpp"
#include "net/addresses.hpp"
#include "net/global_discovery.hpp"
#include "net/local_discovery.hpp"
#include "net/relay.hpp"
#include "platform/platform.hpp"

namespace xst {

using http::Request;
using http::Response;

namespace {

constexpr auto kCsrfLifetime = std::chrono::hours(1);
constexpr auto kSessionLifetime = std::chrono::hours(24 * 7);

Response json_response(const json &j, int status = 200) { return Response::json(j.dump(2) + "\n", status); }
Response error_response(int status, const std::string &msg) { return Response::text(msg + "\n", status); }
Response no_content()
{
	Response r;
	r.status = 200;
	return r;
}

std::string mime_type(const std::string &path)
{
	static const std::map<std::string, std::string> types = {
		{ ".html", "text/html; charset=utf-8" }, { ".js", "application/javascript; charset=utf-8" },
		{ ".css", "text/css; charset=utf-8" }, { ".json", "application/json; charset=utf-8" },
		{ ".png", "image/png" }, { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" },
		{ ".woff", "font/woff" }, { ".woff2", "font/woff2" }, { ".ttf", "font/ttf" }, { ".eot", "application/vnd.ms-fontobject" },
		{ ".txt", "text/plain; charset=utf-8" },
	};
	const auto dot = path.rfind('.');
	if (dot != std::string::npos)
		if (auto it = types.find(path.substr(dot)); it != types.end())
			return it->second;
	return "application/octet-stream";
}

bool is_no_auth_path(const std::string &path)
{
	static const std::vector<std::string> exact = { "/", "/index.html", "/modal.html", "/rest/svc/lang" };
	static const std::vector<std::string> prefixes = { "/assets/", "/syncthing/", "/vendor/", "/theme-assets/",
		"/rest/noauth" };
	if (std::find(exact.begin(), exact.end(), path) != exact.end())
		return true;
	return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string &p) { return path.starts_with(p); });
}

std::optional<DeviceId> device_param(const Request &r, const std::string &name = "device")
{
	if (!r.has_param(name))
		return std::nullopt;
	return DeviceId::parse(r.param(name));
}

std::string random_string(size_t n)
{
	static const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	std::random_device rd;
	std::string s;
	for (size_t i = 0; i < n; i++)
		s.push_back(chars[rd() % 62]);
	return s;
}

// Replaces a list member (folder or device) matched by key, or appends it.
void upsert(json &list, const std::string &key, const json &item)
{
	for (auto &x : list)
		if (x.value(key, "") == item.value(key, "")) {
			x = item;
			return;
		}
	list.push_back(item);
}

// Device IDs in config are canonical text; accept any form from the GUI.
void canonicalize_ids(json &cfg)
{
	for (auto &d : cfg["devices"])
		if (auto id = DeviceId::parse(d.value("deviceID", "")))
			d["deviceID"] = id->to_string();
	for (auto &f : cfg["folders"])
		for (auto &d : f["devices"])
			if (auto id = DeviceId::parse(d.value("deviceID", "")))
				d["deviceID"] = id->to_string();
}

// The GUI sends a plaintext password; store it as bcrypt (as upstream SetPassword does).
void hash_gui_password(json &gui)
{
	const std::string pw = gui.value("password", "");
	if (!pw.empty() && !bcrypt::looks_like_hash(pw))
		gui["password"] = bcrypt::generate(pw, 10);
}

} // namespace

ApiServer::ApiServer(App &app) : app_(app) { short_id_ = app.identity().id.short_string(); }

ApiServer::~ApiServer() { stop(); }

void ApiServer::start()
{
	restart_listener();
	// The GUI's own settings change where and how it listens.
	app_.config().on_change([this] {
		const json gui = app_.config().gui();
		const std::string addr = gui.value("address", "0.0.0.0:8384");
		const bool tls = gui.value("useTLS", false);
		const bool enabled = gui.value("enabled", true);
		bool restart;
		{
			std::lock_guard lock(mu_);
			restart = addr != listen_address_ || tls != listen_tls_ || enabled != (server_ != nullptr);
		}
		if (restart)
			std::thread([this] {
				std::this_thread::sleep_for(std::chrono::milliseconds(500)); // let the PUT's response go out
				restart_listener();
			}).detach();
	});
}

void ApiServer::restart_listener()
{
	std::unique_ptr<http::Server> old;
	{
		std::lock_guard lock(mu_);
		old = std::move(server_);
	}
	if (old)
		old->stop();
	const json gui = app_.config().gui();
	const std::string addr = gui.value("address", "0.0.0.0:8384");
	const bool tls = gui.value("useTLS", false);
	{
		std::lock_guard lock(mu_);
		listen_address_ = addr;
		listen_tls_ = tls;
	}
	if (!gui.value("enabled", true))
		return;
	const auto colon = addr.rfind(':');
	const uint16_t port = static_cast<uint16_t>(std::atoi(addr.c_str() + (colon == std::string::npos ? 0 : colon + 1)));
	const Identity *identity = nullptr;
	if (tls) {
		if (!https_identity_)
			https_identity_ = std::make_unique<Identity>(load_https_identity());
		identity = https_identity_.get();
	}
	auto server = std::make_unique<http::Server>([this](const Request &r) { return handle(r); });
	std::string err;
	if (!server->start(addr.substr(0, colon), port, identity, &err)) {
		platform::log("api: listen on " + addr + ": " + err);
		std::lock_guard lock(mu_);
		error_ = err;
		return;
	}
	platform::log("api: GUI and API listening on " + addr + (tls ? " (https)" : ""));
	std::lock_guard lock(mu_);
	error_.clear();
	server_ = std::move(server);
}

void ApiServer::stop()
{
	app_.events().close(); // releases long-polling /rest/events requests, which would otherwise hold stop() for 60 s
	std::unique_ptr<http::Server> s;
	{
		std::lock_guard lock(mu_);
		s = std::move(server_);
	}
	if (s)
		s->stop();
}

std::string ApiServer::url() const
{
	std::lock_guard lock(mu_);
	if (!server_)
		return "";
	std::string host = listen_address_.substr(0, listen_address_.rfind(':'));
	if (host.empty() || host == "0.0.0.0") {
		const auto ips = lan_ipv4_addresses();
		host = ips.empty() ? "127.0.0.1" : ips.front();
	}
	return std::string(listen_tls_ ? "https" : "http") + "://" + host + ":" + std::to_string(server_->port()) + "/";
}

std::string ApiServer::api_key() const { return app_.config().gui().value("apiKey", ""); }

std::string ApiServer::error() const
{
	std::lock_guard lock(mu_);
	return error_;
}

std::string ApiServer::new_token()
{
	const std::string t = random_string(32);
	return t;
}

// --- auth and CSRF, as lib/api ----------------------------------------------------------------------------------------

bool ApiServer::authorized(const Request &req, Response *out, std::vector<std::pair<std::string, std::string>> &cookies)
{
	const json gui = app_.config().gui();
	const std::string key = gui.value("apiKey", "");
	auto valid_key = [&](const std::string &k) { return !key.empty() && k == key; };
	const std::string auth = req.header("authorization");
	const bool api_key = valid_key(req.header("x-api-key")) ||
		(auth.size() > 7 && (auth.starts_with("Bearer ") || auth.starts_with("bearer ")) && valid_key(auth.substr(7)));

	// User/password sessions, if a user and password are set.
	const std::string user = gui.value("user", "");
	const std::string password = gui.value("password", "");
	const bool auth_enabled = !user.empty() && !password.empty();
	const std::string session_cookie = "sessionid-" + short_id_;
	const auto now = std::chrono::steady_clock::now();
	if (auth_enabled && !api_key) {
		bool session = false;
		{
			std::lock_guard lock(mu_);
			auto it = sessions_.find(req.cookies.count(session_cookie) ? req.cookies.at(session_cookie) : "");
			session = it != sessions_.end() && it->second > now;
		}
		if (!session && auth.starts_with("Basic ")) {
			// Basic auth: base64(user:password).
			static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string decoded;
			int val = 0, bits = -8;
			for (char ch : auth.substr(6)) {
				const auto p = b64.find(ch);
				if (p == std::string::npos)
					break;
				val = (val << 6) + static_cast<int>(p);
				bits += 6;
				if (bits >= 0) {
					decoded.push_back(static_cast<char>((val >> bits) & 0xFF));
					bits -= 8;
				}
			}
			const auto colon = decoded.find(':');
			if (colon != std::string::npos && decoded.substr(0, colon) == user &&
				bcrypt::verify(decoded.substr(colon + 1), password)) {
				const std::string sid = new_token();
				std::lock_guard lock(mu_);
				sessions_[sid] = now + kSessionLifetime;
				cookies.emplace_back("Set-Cookie", session_cookie + "=" + sid + "; Path=/; HttpOnly");
				session = true;
			}
		}
		if (!session && !is_no_auth_path(req.path)) {
			*out = error_response(403, "Forbidden");
			return false;
		}
	}

	if (api_key)
		return true;
	// Outside /rest: hand out a CSRF cookie if the browser has no valid one.
	const std::string csrf_cookie = "CSRF-Token-" + short_id_;
	if (!req.path.starts_with("/rest")) {
		std::lock_guard lock(mu_);
		auto it = csrf_tokens_.find(req.cookies.count(csrf_cookie) ? req.cookies.at(csrf_cookie) : "");
		if (it == csrf_tokens_.end() || it->second < now) {
			// Keep at most 25 live tokens, as upstream.
			while (csrf_tokens_.size() >= 25)
				csrf_tokens_.erase(std::min_element(csrf_tokens_.begin(), csrf_tokens_.end(),
					[](auto &a, auto &b) { return a.second < b.second; }));
			const std::string t = random_string(32);
			csrf_tokens_[t] = now + kCsrfLifetime;
			cookies.emplace_back("Set-Cookie", csrf_cookie + "=" + t + "; Path=/");
		}
		return true;
	}
	if (req.path.starts_with("/rest/noauth") || req.path == "/rest/svc/lang")
		return true;
	{
		std::lock_guard lock(mu_);
		auto it = csrf_tokens_.find(req.header("x-csrf-token-" + [&] {
			std::string s = short_id_;
			for (auto &c : s)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}()));
		if (it != csrf_tokens_.end() && it->second > now)
			return true;
	}
	*out = error_response(403, "CSRF Error");
	return false;
}

Response ApiServer::handle(const Request &req)
{
	std::vector<std::pair<std::string, std::string>> cookies;
	Response resp;
	// CORS preflight.
	if (req.method == "OPTIONS") {
		resp.status = 204;
		resp.set("Access-Control-Allow-Origin", "*");
		resp.set("Access-Control-Allow-Headers", "Content-Type, X-API-Key");
		resp.set("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
		return resp;
	}
	if (authorized(req, &resp, cookies)) {
		resp = req.path.starts_with("/rest/") ? route(req) : serve_static(req);
		if (!req.header("x-api-key").empty())
			resp.set("Access-Control-Allow-Origin", "*");
	}
	for (auto &c : cookies)
		resp.headers.push_back(c);
	resp.set("X-Syncthing-Version", std::string(client_version()));
	resp.set("X-Syncthing-ID", app_.identity().id.to_string());
	resp.set("Cache-Control", "no-cache, no-store, must-revalidate");
	return resp;
}

Response ApiServer::serve_static(const Request &req)
{
	if (req.path == "/meta.js") {
		Response r;
		r.body = "var metadata = " +
			json{ { "deviceID", app_.identity().id.to_string() }, { "deviceIDShort", short_id_ }, { "authenticated", true } }
				.dump() +
			";\n";
		r.set("Content-Type", "application/javascript");
		return r;
	}
	const std::string dir = app_.options().gui_assets_dir;
	if (dir.empty())
		return error_response(404, "the Web GUI is not installed");
	std::string file = req.path == "/" ? "index.html" : req.path.substr(1);
	if (file.find("..") != std::string::npos)
		return error_response(400, "bad path");
	if (file == "themes.json") {
		json themes = json::array();
		for (const char *t : { "black", "dark", "default", "light" }) {
			struct stat st;
			if (::stat((dir + "/" + t).c_str(), &st) == 0)
				themes.push_back(t);
		}
		return json_response({ { "themes", themes } });
	}
	// As lib/api/api_statics.go: "theme-assets/<theme>/<file>" names a theme; otherwise the configured one.
	std::string theme = app_.config().gui().value("theme", "default");
	if (file.starts_with("theme-assets/")) {
		const std::string rest = file.substr(13);
		const auto slash = rest.find('/');
		if (slash == std::string::npos)
			return error_response(404, "not found");
		theme = rest.substr(0, slash);
		file = rest.substr(slash + 1);
	}
	for (const std::string &t : { theme, std::string("default") }) {
		std::ifstream in(dir + "/" + t + "/" + file, std::ios::binary);
		if (!in)
			continue;
		std::stringstream ss;
		ss << in.rdbuf();
		Response r;
		r.body = ss.str();
		r.set("Content-Type", mime_type(file));
		return r;
	}
	return error_response(404, "not found");
}

} // namespace xst

namespace xst {

namespace {

Identity load_https_identity_impl(const std::string &dir)
{
	std::string err;
	auto read = [](const std::string &p) {
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	};
	const std::string cert = read(dir + "/https-cert.pem"), key = read(dir + "/https-key.pem");
	if (!cert.empty() && !key.empty())
		if (auto id = Identity::from_pem(cert, key, &err))
			return *id;
	auto id = Identity::generate_ecdsa_p256(&err, "syncthing", 820);
	if (id) {
		std::ofstream(dir + "/https-cert.pem", std::ios::binary) << id->cert_pem();
		std::ofstream(dir + "/https-key.pem", std::ios::binary) << id->key_pem();
		return *id;
	}
	platform::log("api: generating the HTTPS certificate failed: " + err);
	return Identity{};
}

json connection_json(const PeerConnection &c)
{
	const auto started = std::chrono::duration_cast<std::chrono::seconds>(c.started().time_since_epoch()).count();
	json j = { { "at", rfc3339_now() }, { "inBytesTotal", c.in_bytes() }, { "outBytesTotal", c.out_bytes() },
		{ "startedAt", rfc3339(started) }, { "address", c.address() }, { "type", c.type() },
		{ "isLocal", c.is_local() }, { "crypto", c.crypto() } };
	return j;
}

json empty_connection()
{
	return { { "at", "0001-01-01T00:00:00Z" }, { "inBytesTotal", 0 }, { "outBytesTotal", 0 },
		{ "startedAt", "0001-01-01T00:00:00Z" }, { "address", "" }, { "type", "" }, { "isLocal", false },
		{ "crypto", "" } };
}

} // namespace

Identity ApiServer::load_https_identity() { return load_https_identity_impl(app_.options().data_dir); }

Response ApiServer::route(const Request &req)
{
	const std::string &p = req.path;
	const std::string &m = req.method;
	Config &cfg = app_.config();
	Model &model = app_.model();
	const bool get = m == "GET";
	auto body_json = [&]() -> std::optional<json> {
		try {
			return json::parse(req.body.empty() ? "{}" : req.body);
		} catch (...) {
			return std::nullopt;
		}
	};

	// --- /rest/config ---------------------------------------------------------------------------------------------
	if (p == "/rest/config" || p == "/rest/system/config") {
		if (get)
			return json_response(cfg.get());
		if (m == "PUT" || m == "POST") {
			auto j = body_json();
			if (!j)
				return error_response(400, "bad JSON");
			canonicalize_ids(*j);
			hash_gui_password((*j)["gui"]);
			// Keep the API key if the GUI sent an empty one.
			if ((*j)["gui"].value("apiKey", "").empty())
				(*j)["gui"]["apiKey"] = cfg.gui().value("apiKey", "");
			cfg.replace(*j);
			return no_content();
		}
	}
	if (p == "/rest/config/insync" || p == "/rest/system/config/insync")
		return json_response({ { "configInSync", true } });
	if (p == "/rest/config/restart-required")
		return json_response({ { "requiresRestart", false } });
	if (p == "/rest/config/folders" || p == "/rest/config/devices") {
		const bool folders = p.ends_with("folders");
		const std::string list = folders ? "folders" : "devices";
		const std::string key = folders ? "id" : "deviceID";
		if (get)
			return json_response(cfg.get()[list]);
		auto j = body_json();
		if (!j)
			return error_response(400, "bad JSON");
		if (m == "PUT") {
			cfg.modify([&](json &c) {
				c[list] = *j;
				canonicalize_ids(c);
			});
			return no_content();
		}
		if (m == "POST") {
			json item = *j;
			if (folders) {
				json d = cfg.defaults()["folder"];
				d.update(item);
				item = d;
				// Always share with ourselves.
				bool self = false;
				for (auto &fd : item["devices"])
					if (DeviceId::parse(fd.value("deviceID", "")) == cfg.my_id())
						self = true;
				if (!self)
					item["devices"].push_back({ { "deviceID", cfg.my_id().to_string() }, { "introducedBy", "" },
						{ "encryptionPassword", "" } });
			} else {
				json d = cfg.defaults()["device"];
				d.update(item);
				item = d;
				if (auto id = DeviceId::parse(item.value("deviceID", "")))
					item["deviceID"] = id->to_string();
				else
					return error_response(400, "invalid device ID");
			}
			cfg.modify([&](json &c) {
				upsert(c[list], key, item);
				canonicalize_ids(c);
			});
			if (!folders)
				app_.connections().dial_now();
			return no_content();
		}
	}
	for (const char *base : { "/rest/config/folders/", "/rest/config/devices/" }) {
		if (!p.starts_with(base))
			continue;
		const bool folders = std::string(base).find("folders") != std::string::npos;
		const std::string list = folders ? "folders" : "devices";
		const std::string key = folders ? "id" : "deviceID";
		std::string id = p.substr(std::string(base).size());
		if (!folders) {
			auto did = DeviceId::parse(id);
			if (!did)
				return error_response(400, "invalid device ID");
			id = did->to_string();
		}
		json current = nullptr;
		for (const auto &x : cfg.get()[list])
			if (x.value(key, "") == id)
				current = x;
		if (get)
			return current.is_null() ? error_response(404, "No such object in the configuration.") : json_response(current);
		if (m == "DELETE") {
			if (current.is_null())
				return error_response(404, "No such object in the configuration.");
			cfg.modify([&](json &c) {
				auto &l = c[list];
				l.erase(std::remove_if(l.begin(), l.end(), [&](const json &x) { return x.value(key, "") == id; }), l.end());
				if (!folders) // a removed device is unshared from every folder
					for (auto &f : c["folders"]) {
						auto &fd = f["devices"];
						fd.erase(std::remove_if(fd.begin(), fd.end(),
									 [&](const json &x) { return DeviceId::parse(x.value("deviceID", "")) == DeviceId::parse(id); }),
							fd.end());
					}
			});
			if (folders)
				app_.model().db().drop_folder(id);
			return no_content();
		}
		auto j = body_json();
		if (!j)
			return error_response(400, "bad JSON");
		json item = m == "PATCH" ? (current.is_null() ? json::object() : current) : json::object();
		if (m == "PATCH" && current.is_null())
			return error_response(404, "No such object in the configuration.");
		if (m == "PUT")
			item = folders ? cfg.defaults()["folder"] : cfg.defaults()["device"];
		item.update(*j);
		item[key] = id;
		cfg.modify([&](json &c) {
			upsert(c[list], key, item);
			canonicalize_ids(c);
		});
		return no_content();
	}
	for (const char *section : { "options", "gui", "ldap" }) {
		if (p != std::string("/rest/config/") + section)
			continue;
		if (get)
			return json_response(cfg.get().value(section, json::object()));
		auto j = body_json();
		if (!j)
			return error_response(400, "bad JSON");
		cfg.modify([&](json &c) {
			if (m == "PUT")
				c[section] = *j;
			else
				c[section].update(*j);
			if (std::string(section) == "gui") {
				hash_gui_password(c["gui"]);
				if (c["gui"].value("apiKey", "").empty())
					c["gui"]["apiKey"] = random_string(32);
			}
		});
		return no_content();
	}
	if (p == "/rest/config/defaults/folder" || p == "/rest/config/defaults/device") {
		const std::string which = p.ends_with("folder") ? "folder" : "device";
		if (get)
			return json_response(cfg.defaults()[which]);
		auto j = body_json();
		if (!j)
			return error_response(400, "bad JSON");
		cfg.modify([&](json &c) {
			if (m == "PUT")
				c["defaults"][which] = *j;
			else
				c["defaults"][which].update(*j);
		});
		return no_content();
	}
	if (p == "/rest/config/defaults/ignores") {
		if (get)
			return json_response(cfg.defaults().value("ignores", json{ { "lines", json::array() } }));
		auto j = body_json();
		if (!j)
			return error_response(400, "bad JSON");
		cfg.modify([&](json &c) { c["defaults"]["ignores"] = *j; });
		return no_content();
	}

	// --- /rest/system -----------------------------------------------------------------------------------------------
	if (p == "/rest/system/ping")
		return json_response({ { "ping", "pong" } });
	if (p == "/rest/noauth/health")
		return json_response({ { "status", "OK" } });
	if (p == "/rest/system/version")
		return json_response({ { "arch",
#if defined(__XSDK__)
									 "ppc"
#else
									 "amd64"
#endif
								 }, { "codename", "Xbox 360" }, { "container", false }, { "date", "" },
			{ "extra", "xsyncthing" }, { "isBeta", false }, { "isCandidate", false }, { "isRelease", true },
			{ "longVersion", std::string(client_name()) + " " + std::string(client_version()) +
#if defined(__XSDK__)
					" (xbox360-ppc, xsdk)"
#else
					" (linux, host build)"
#endif
			},
			{ "os",
#if defined(__XSDK__)
				"xbox360"
#else
				"linux"
#endif
			},
			{ "stamp", "0" }, { "tags", json::array({ "xsyncthing" }) }, { "user", "" },
			{ "version", std::string(client_version()) } });
	if (p == "/rest/system/status") {
		json disco_status = json::object(), disco_errors = json::object();
		if (auto *ld = app_.local_discovery()) {
			const std::string e = ld->error();
			disco_status["IPv4 local"] = { { "error", e.empty() ? json(nullptr) : json(e) } };
			if (!e.empty())
				disco_errors["IPv4 local"] = e;
		}
		if (auto *gd = app_.global_discovery())
			for (const auto &[server, e] : gd->errors()) {
				disco_status["global@" + server] = { { "error", e.empty() ? json(nullptr) : json(e) } };
				if (!e.empty())
					disco_errors["global@" + server] = e;
			}
		json listeners = app_.connections().listener_status();
		if (auto *r = app_.relays()) {
			const std::string joined = r->joined_uri();
			const std::string e = r->error();
			listeners["dynamic+https://relays.syncthing.net/endpoint"] = { { "error", joined.empty() && !e.empty() ? json(e) : json(nullptr) },
				{ "lanAddresses", joined.empty() ? json::array() : json::array({ joined }) },
				{ "wanAddresses", joined.empty() ? json::array() : json::array({ joined }) } };
		}
		const auto started = std::chrono::system_clock::now();
		(void)started;
		return json_response({ { "alloc", 0 }, { "connectionServiceStatus", listeners }, { "cpuPercent", 0 },
			{ "discoveryEnabled", true }, { "discoveryErrors", disco_errors }, { "discoveryMethods", disco_status.size() },
			{ "discoveryStatus", disco_status }, { "goroutines", 0 }, { "guiAddressOverridden", false },
			{ "guiAddressUsed", cfg.gui().value("address", "") }, { "lastDialStatus", json::object() },
			{ "myID", cfg.my_id().to_string() }, { "pathSeparator", "/" }, { "startTime", model.started_at() },
			{ "sys", 0 }, { "tilde", platform::data_dir() }, { "uptime", 0 }, { "urVersionMax", 3 } });
	}
	if (p == "/rest/system/connections") {
		json conns = json::object();
		for (const auto &d : cfg.devices()) {
			const auto id = DeviceId::parse(d.value("deviceID", ""));
			if (!id || *id == cfg.my_id())
				continue;
			json c = empty_connection();
			c["connected"] = false;
			c["paused"] = d.value("paused", false);
			c["clientVersion"] = "";
			c["primary"] = empty_connection();
			conns[id->to_string()] = c;
		}
		for (const auto &c : model.connections()) {
			json j = connection_json(*c);
			j["connected"] = true;
			j["paused"] = false;
			j["clientVersion"] = c->client_version();
			j["primary"] = connection_json(*c);
			conns[c->device().to_string()] = j;
		}
		return json_response({ { "connections", conns },
			{ "total", { { "at", rfc3339_now() }, { "inBytesTotal", model.total_in() }, { "outBytesTotal", model.total_out() } } } });
	}
	if (p == "/rest/system/discovery") {
		json out = json::object();
		if (auto *ld = app_.local_discovery())
			for (const auto &[id, addrs] : ld->cache())
				out[id.to_string()] = { { "addresses", addrs } };
		return json_response(out);
	}
	if (p == "/rest/system/error") {
		if (get)
			return json_response({ { "errors", nullptr } });
		return no_content();
	}
	if (p == "/rest/system/error/clear")
		return no_content();
	if (p == "/rest/system/log" || p == "/rest/system/log.txt") {
		const auto lines = platform::recent_log();
		if (p.ends_with(".txt")) {
			std::string t;
			for (const auto &l : lines)
				t += l.when + ": " + l.message + "\n";
			return Response::text(t, 200);
		}
		json msgs = json::array();
		for (const auto &l : lines)
			msgs.push_back({ { "when", l.when }, { "message", l.message }, { "level", "INF" } });
		return json_response({ { "messages", msgs } });
	}
	if (p == "/rest/system/loglevels") {
		if (get)
			return json_response({ { "levels", { { "main", "INFO" } } }, { "packages", { { "main", "Main package" } } } });
		return no_content();
	}
	if (p == "/rest/system/paths")
		return json_response({ { "baseDir-config", platform::data_dir() }, { "baseDir-data", platform::data_dir() },
			{ "certFile", platform::data_dir() + "/cert.pem" }, { "keyFile", platform::data_dir() + "/key.pem" },
			{ "config", platform::data_dir() + "/config.json" }, { "database", platform::data_dir() + "/index.db" },
			{ "guiAssets", app_.options().gui_assets_dir },
			{ "httpsCertFile", platform::data_dir() + "/https-cert.pem" },
			{ "httpsKeyFile", platform::data_dir() + "/https-key.pem" },
			{ "defFolder", cfg.defaults()["folder"].value("path", "") } });
	if (p == "/rest/system/browse") {
		// Directories starting with `current` (the folder-path autocomplete), as upstream.
		std::string current = req.param("current");
		json out = json::array();
		if (current.empty()) {
			for (const char *root : { "hdd:/", "usb0:/", "usb1:/", "usb2:/" }) {
				struct stat st;
				if (::stat(root, &st) == 0)
					out.push_back(root);
			}
#if !defined(__XSDK__)
			out = json::array({ "/" });
#endif
			return json_response(out);
		}
		const auto slash = current.rfind('/');
		const std::string dir = slash == std::string::npos ? current : current.substr(0, slash + 1);
		const std::string prefix = slash == std::string::npos ? "" : current.substr(slash + 1);
		std::vector<std::string> found;
		if (DIR *d = ::opendir(dir.c_str())) {
			while (dirent *e = ::readdir(d)) {
				const std::string n = e->d_name;
				if (n == "." || n == ".." || !n.starts_with(prefix))
					continue;
				struct stat st;
				if (::stat((dir + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
					found.push_back(dir + n + "/");
			}
			::closedir(d);
		}
		std::sort(found.begin(), found.end());
		return json_response(found);
	}
	if (p == "/rest/system/restart" || p == "/rest/system/shutdown" || p == "/rest/system/reset") {
		if (m != "POST")
			return error_response(405, "use POST");
		if (p.ends_with("reset"))
			for (const auto &f : cfg.folders())
				model.db().drop_folder(f["id"]);
		platform::request_restart(p.ends_with("shutdown") ? platform::RestartKind::shutdown : platform::RestartKind::restart);
		return json_response({ { "ok", p.ends_with("shutdown") ? "shutting down" : "restarting" } });
	}
	if (p == "/rest/system/pause" || p == "/rest/system/resume") {
		const bool pause = p.ends_with("pause");
		const auto dev = device_param(req);
		cfg.modify([&](json &c) {
			for (auto &d : c["devices"])
				if (!dev || DeviceId::parse(d.value("deviceID", "")) == *dev)
					if (DeviceId::parse(d.value("deviceID", "")) != cfg.my_id())
						d["paused"] = pause;
		});
		return no_content();
	}
	if (p == "/rest/system/upgrade") {
		// No self-upgrade on the console: releases are new .xex files (docs/COMPAT.md).
		if (get)
			return json_response({ { "latest", std::string(client_version()) }, { "majorNewer", false }, { "newer", false },
				{ "running", std::string(client_version()) } });
		return error_response(501, "upgrade unsupported");
	}

	// --- /rest/svc ------------------------------------------------------------------------------------------------
	if (p == "/rest/svc/lang")
		return json_response(json::array({ "en" }));
	if (p == "/rest/svc/deviceid") {
		const auto id = DeviceId::parse(req.param("id"));
		if (!id || req.param("id").empty())
			return json_response({ { "error", "device ID invalid: incorrect length" } });
		return json_response({ { "id", id->to_string() } });
	}
	if (p == "/rest/svc/random/string") {
		const int len = std::clamp(std::atoi(req.param("length", "32").c_str()), 1, 1024);
		return json_response({ { "random", random_string(static_cast<size_t>(len)) } });
	}
	if (p == "/rest/svc/report")
		// Usage reporting is never sent from xsyncthing; this is what it would contain.
		return json_response({ { "version", std::string(client_version()) }, { "platform", "xbox360-ppc" },
			{ "numFolders", cfg.folders().size() }, { "numDevices", cfg.devices().size() }, { "urVersion", 3 },
			{ "uniqueID", "" }, { "longVersion", std::string(client_name()) + " " + std::string(client_version()) } });

	// --- /rest/cluster ----------------------------------------------------------------------------------------------
	if (p == "/rest/cluster/pending/devices") {
		if (get)
			return json_response(model.pending_devices());
		if (m == "DELETE") {
			const auto d = device_param(req);
			if (!d)
				return error_response(400, "device ID invalid");
			model.dismiss_pending_device(*d);
			return no_content();
		}
	}
	if (p == "/rest/cluster/pending/folders") {
		const auto d = device_param(req);
		if (get)
			return json_response(model.pending_folders(d.value_or(DeviceId())));
		if (m == "DELETE") {
			model.dismiss_pending_folder(req.param("folder"), d.value_or(DeviceId()));
			return no_content();
		}
	}

	// --- /rest/db ---------------------------------------------------------------------------------------------------
	const std::string folder = req.param("folder");
	const bool folder_known = !cfg.folder(folder).is_null();
	if (p == "/rest/db/status") {
		if (!folder_known)
			return error_response(404, "Folder \"" + folder + "\" does not exist");
		return json_response(model.folder_status(folder));
	}
	if (p == "/rest/db/completion") {
		DeviceId dev = cfg.my_id();
		if (req.has_param("device")) {
			const auto d = device_param(req);
			if (!d)
				return error_response(400, "device ID invalid");
			dev = *d;
		}
		return json_response(model.completion(folder, dev));
	}
	if (p == "/rest/db/scan") {
		if (!folder.empty()) {
			if (!folder_known)
				return error_response(500, "Folder \"" + folder + "\" does not exist");
			model.scan(folder, req.param("sub"));
		} else {
			model.scan_all();
		}
		return no_content();
	}
	if (p == "/rest/db/ignores") {
		if (!folder_known)
			return error_response(500, "Folder \"" + folder + "\" does not exist");
		if (m == "POST") {
			auto j = body_json();
			if (!j)
				return error_response(400, "bad JSON");
			std::vector<std::string> lines;
			for (const auto &l : (*j).value("ignore", json::array()))
				lines.push_back(l.get<std::string>());
			model.set_ignores(folder, lines);
		}
		std::string err;
		const auto lines = model.ignores(folder, &err);
		return json_response({ { "ignore", lines.empty() ? json(nullptr) : json(lines) },
			{ "expanded", lines.empty() ? json(nullptr) : json(lines) },
			{ "error", err.empty() ? json(nullptr) : json(err) } });
	}
	if (p == "/rest/db/need" || p == "/rest/db/prio") {
		const int page = std::max(1, std::atoi(req.param("page", "1").c_str()));
		const int per = std::max(1, std::atoi(req.param("perpage", "65536").c_str()));
		return json_response(model.need(folder, page, per));
	}
	if (p == "/rest/db/remoteneed" || p == "/rest/db/localchanged") {
		return json_response({ { "files", json::array() }, { "page", 1 }, { "perpage", 65536 } });
	}
	if (p == "/rest/db/browse")
		return json_response(model.browse(folder, req.param("prefix"), std::atoi(req.param("levels", "-1").c_str())));
	if (p == "/rest/db/file") {
		const json j = model.file_info(folder, req.param("file"));
		if (!j.contains("global") && !j.contains("local"))
			return error_response(404, "no such object in the index");
		return json_response(j);
	}
	if (p == "/rest/db/override") {
		model.override_folder(folder);
		return no_content();
	}
	if (p == "/rest/db/revert") {
		model.revert_folder(folder);
		return no_content();
	}

	// --- /rest/folder, /rest/stats --------------------------------------------------------------------------------------
	if (p == "/rest/folder/errors" || p == "/rest/folder/pullerrors") {
		const json errs = model.folder_errors(folder);
		return json_response({ { "errors", errs.empty() ? json(nullptr) : errs }, { "folder", folder }, { "page", 1 },
			{ "perpage", 65536 } });
	}
	if (p == "/rest/folder/versions")
		return error_response(500, "folder has no versioner");
	if (p == "/rest/stats/device")
		return json_response(model.device_stats());
	if (p == "/rest/stats/folder")
		return json_response(model.folder_stats());

	// --- /rest/events -----------------------------------------------------------------------------------------------
	if (p == "/rest/events" || p == "/rest/events/disk") {
		const int64_t since = std::atoll(req.param("since", "0").c_str());
		const int timeout_s = std::clamp(std::atoi(req.param("timeout", "60").c_str()), 0, 600);
		const size_t limit = static_cast<size_t>(std::max(0, std::atoi(req.param("limit", "0").c_str())));
		std::vector<std::string> types;
		if (p.ends_with("disk"))
			types = { "LocalChangeDetected", "RemoteChangeDetected" };
		else if (req.has_param("events")) {
			std::stringstream ss(req.param("events"));
			std::string t;
			while (std::getline(ss, t, ','))
				if (!t.empty())
					types.push_back(t);
		} else {
			// The default mask leaves out the noisy ones, as upstream's DefaultEventMask.
			types = { "Starting", "StartupComplete", "ConfigSaved", "DeviceConnected", "DeviceDisconnected",
				"DeviceDiscovered", "DevicePaused", "DeviceResumed", "DeviceRejected", "PendingDevicesChanged",
				"PendingFoldersChanged", "FolderRejected", "FolderCompletion", "FolderErrors", "FolderScanProgress",
				"FolderSummary", "FolderPaused", "FolderResumed", "FolderWatchStateChanged", "ItemStarted",
				"ItemFinished", "LocalIndexUpdated", "RemoteIndexUpdated", "StateChanged", "ClusterConfigReceived",
				"DownloadProgress", "ListenAddressesChanged", "LoginAttempt", "Failure" };
		}
		// Events missed before a disconnect are fetched with since; a first poll (since=0) gets the recent ones.
		const auto events = app_.events().since(since, types, std::chrono::seconds(timeout_s), limit);
		json out = json::array();
		for (const auto &e : events)
			out.push_back(Events::to_json(e));
		return json_response(out);
	}

	if (p == "/rest/noauth/auth/password") {
		auto j = body_json();
		const json gui = cfg.gui();
		if (j && (*j).value("username", (*j).value("Username", "")) == gui.value("user", "") &&
			bcrypt::verify((*j).value("password", (*j).value("Password", "")), gui.value("password", ""))) {
			const std::string sid = new_token();
			{
				std::lock_guard lock(mu_);
				sessions_[sid] = std::chrono::steady_clock::now() + kSessionLifetime;
			}
			Response r;
			r.status = 204;
			r.set("Set-Cookie", "sessionid-" + short_id_ + "=" + sid + "; Path=/; HttpOnly");
			return r;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // anti brute force, as upstream
		return error_response(403, "Forbidden");
	}
	if (p == "/rest/noauth/auth/logout") {
		Response r;
		r.status = 204;
		r.set("Set-Cookie", "sessionid-" + short_id_ + "=; Path=/; Max-Age=0");
		return r;
	}
	if (p.starts_with("/rest/debug"))
		return error_response(404, "debugging is not enabled");
	return error_response(404, "Not Found");
}

} // namespace xst
