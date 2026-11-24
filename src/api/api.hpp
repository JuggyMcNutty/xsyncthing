#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "api/http_server.hpp"
#include "app.hpp"

namespace xst {

// The REST API and the Web GUI (src/api), as lib/api: the stock GUI's files from gui_assets_dir, /rest/* backed by
// the in-process model (the console UI reads the same model), API-key and CSRF protection, optional user/password
// sessions, and /rest/events long-polling.
class ApiServer : public Service {
public:
	explicit ApiServer(App &app);
	~ApiServer() override;

	void start() override;
	void stop() override;

	// For the console UI: where the GUI listens ("" if off) and how to log in.
	std::string url() const;
	std::string api_key() const;
	std::string error() const;

	// Exposed for tests: handle one request.
	http::Response handle(const http::Request &req);

private:
	struct Route;
	http::Response route(const http::Request &req);
	http::Response serve_static(const http::Request &req);
	bool authorized(const http::Request &req, http::Response *out, std::vector<std::pair<std::string, std::string>> &cookies);
	std::string new_token();
	void restart_listener();
	Identity load_https_identity();

	App &app_;
	std::unique_ptr<http::Server> server_;
	std::unique_ptr<Identity> https_identity_;
	std::string short_id_;
	mutable std::mutex mu_;
	std::map<std::string, std::chrono::steady_clock::time_point> csrf_tokens_;
	std::map<std::string, std::chrono::steady_clock::time_point> sessions_;
	std::string listen_address_, error_;
	bool listen_tls_ = false;
	std::vector<std::string> log_lines_;
};

} // namespace xst
