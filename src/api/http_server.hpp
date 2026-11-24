#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/identity.hpp"
#include "net/tcp.hpp"
#include "net/tls.hpp"

namespace xst::http {

struct Request {
	std::string method;
	std::string path;  // decoded, without the query
	std::string query; // raw
	std::map<std::string, std::string> params;  // query parameters, decoded
	std::map<std::string, std::string> headers; // lower-case names
	std::map<std::string, std::string> cookies;
	std::string body;
	std::string remote;
	bool tls = false;

	std::string header(const std::string &name) const
	{
		auto it = headers.find(name);
		return it == headers.end() ? "" : it->second;
	}
	std::string param(const std::string &name, const std::string &fallback = "") const
	{
		auto it = params.find(name);
		return it == params.end() ? fallback : it->second;
	}
	bool has_param(const std::string &name) const { return params.count(name) > 0; }
};

struct Response {
	int status = 200;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;

	void set(const std::string &k, const std::string &v) { headers.emplace_back(k, v); }
	static Response json(const std::string &body, int status = 200)
	{
		Response r;
		r.status = status;
		r.body = body;
		r.set("Content-Type", "application/json; charset=utf-8");
		return r;
	}
	static Response text(const std::string &body, int status)
	{
		Response r;
		r.status = status;
		r.body = body;
		r.set("Content-Type", "text/plain; charset=utf-8");
		return r;
	}
};

using Handler = std::function<Response(const Request &)>;

// A small threaded HTTP/1.1 server (keep-alive, Content-Length bodies, optionally TLS). One thread per connection,
// up to max_connections; the GUI holds a few (one long-polling /rest/events).
class Server {
public:
	Server(Handler handler, int max_connections = 16) : handler_(std::move(handler)), max_conns_(max_connections) {}
	~Server();

	// Listens on host:port ("0.0.0.0" or "127.0.0.1"). With identity, serves HTTPS.
	bool start(const std::string &host, uint16_t port, const Identity *tls_identity, std::string *error);
	void stop();
	uint16_t port() const { return port_; }

private:
	void accept_loop();
	void serve(std::unique_ptr<Stream> stream, std::string remote);

	Handler handler_;
	int max_conns_;
	const Identity *tls_identity_ = nullptr;
	std::atomic<bool> stopping_{ false };
	std::atomic<int> active_{ 0 };
	std::atomic<uint16_t> port_{ 0 };
	std::unique_ptr<TcpListener> listener_;
	std::thread thread_;
	std::mutex mu_;
	struct ConnThread {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::vector<ConnThread> conns_;
	std::vector<Stream *> streams_;
};

std::string status_text(int status);
std::string url_decode(const std::string &s);

} // namespace xst::http
