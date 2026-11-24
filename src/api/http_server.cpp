#include "api/http_server.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "net/tcp.hpp"
#include "platform/platform.hpp"

namespace xst::http {

namespace {

constexpr size_t kMaxHeader = 64 * 1024;
constexpr size_t kMaxBody = 16 * 1024 * 1024;
constexpr int kIdleTimeoutMs = 120000; // longer than /rest/events' 60 s poll

std::string lower(std::string s)
{
	for (auto &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string trim(std::string s)
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
		i++;
	return s.substr(i);
}

// Bytes in and out over TLS or plain TCP.
struct Conn {
	Stream *plain = nullptr;
	TlsConnection *tls = nullptr;
	std::string buf;
	bool fill()
	{
		uint8_t chunk[16384];
		const long n = tls ? tls->read(chunk) : plain->read(chunk);
		if (n <= 0)
			return false;
		buf.append(reinterpret_cast<char *>(chunk), static_cast<size_t>(n));
		return true;
	}
	bool write(const std::string &s)
	{
		const std::span<const uint8_t> b(reinterpret_cast<const uint8_t *>(s.data()), s.size());
		if (tls)
			return tls->write_all(b);
		size_t done = 0;
		while (done < b.size()) {
			const long n = plain->write(b.subspan(done));
			if (n <= 0)
				return false;
			done += static_cast<size_t>(n);
		}
		return true;
	}
};

} // namespace

std::string url_decode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
			std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
			i += 2;
		} else if (s[i] == '+') {
			out.push_back(' ');
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

std::string status_text(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 301:
		return "Moved Permanently";
	case 302:
		return "Found";
	case 304:
		return "Not Modified";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Payload Too Large";
	case 500:
		return "Internal Server Error";
	case 501:
		return "Not Implemented";
	case 503:
		return "Service Unavailable";
	default:
		return "Status";
	}
}

Server::~Server() { stop(); }

bool Server::start(const std::string &host, uint16_t port, const Identity *tls_identity, std::string *error)
{
	(void)host; // TcpListener binds all interfaces; the API layer checks the Host for localhost-only setups
	tls_identity_ = tls_identity;
	listener_ = TcpListener::listen(port, error);
	if (!listener_)
		return false;
	port_ = listener_->port();
	thread_ = std::thread([this] { accept_loop(); });
	return true;
}

void Server::stop()
{
	if (stopping_.exchange(true))
		return;
	if (listener_)
		listener_->shutdown();
	{
		std::lock_guard lock(mu_);
		for (auto *s : streams_)
			s->interrupt();
	}
	if (thread_.joinable())
		thread_.join();
	std::vector<ConnThread> conns;
	{
		std::lock_guard lock(mu_);
		conns.swap(conns_);
	}
	for (auto &c : conns)
		if (c.thread.joinable())
			c.thread.join();
	listener_.reset();
}

void Server::accept_loop()
{
	while (!stopping_) {
		std::string remote;
		auto stream = listener_->accept(&remote, 500);
		if (!stream)
			continue;
		if (active_ >= max_conns_) {
			stream->close();
			continue;
		}
		std::lock_guard lock(mu_);
		// Join connections that have finished, so handles don't pile up over a long run.
		for (auto it = conns_.begin(); it != conns_.end();) {
			if (it->done->load()) {
				it->thread.join();
				it = conns_.erase(it);
			} else {
				++it;
			}
		}
		active_++;
		auto done = std::make_shared<std::atomic<bool>>(false);
		conns_.push_back({ std::thread([this, s = std::move(stream), remote, done]() mutable {
			serve(std::move(s), remote);
			*done = true;
		}),
			done });
	}
}

void Server::serve(std::unique_ptr<Stream> stream, std::string remote)
{
	struct Done {
		Server *s;
		Stream *raw;
		~Done()
		{
			std::lock_guard lock(s->mu_);
			s->streams_.erase(std::remove(s->streams_.begin(), s->streams_.end(), raw), s->streams_.end());
			s->active_--;
		}
	} done{ this, stream.get() };
	{
		std::lock_guard lock(mu_);
		streams_.push_back(stream.get());
	}
	static_cast<TcpStream *>(stream.get())->set_timeout(kIdleTimeoutMs);

	Conn c;
	std::unique_ptr<TlsConnection> tls;
	if (tls_identity_) {
		TlsOptions o;
		o.role = TlsOptions::Role::server;
		o.min_version = 12;
		o.identity = tls_identity_;
		o.require_peer_cert = false;
		o.alpn = {};
		std::string e;
		tls = TlsConnection::handshake(std::move(stream), o, &e);
		if (!tls)
			return;
		c.tls = tls.get();
	} else {
		c.plain = stream.get();
	}

	while (!stopping_) {
		// Headers.
		size_t end;
		while ((end = c.buf.find("\r\n\r\n")) == std::string::npos) {
			if (c.buf.size() > kMaxHeader || !c.fill())
				return;
		}
		std::istringstream head(c.buf.substr(0, end));
		c.buf.erase(0, end + 4);
		Request req;
		req.remote = remote;
		req.tls = tls != nullptr;
		std::string line, target, version;
		std::getline(head, line);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		{
			std::istringstream rl(line);
			rl >> req.method >> target >> version;
		}
		while (std::getline(head, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const auto colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			req.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
		}
		const auto q = target.find('?');
		req.path = url_decode(target.substr(0, q));
		if (q != std::string::npos) {
			req.query = target.substr(q + 1);
			std::istringstream qs(req.query);
			std::string kv;
			while (std::getline(qs, kv, '&')) {
				if (kv.empty())
					continue;
				const auto eq = kv.find('=');
				req.params[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
			}
		}
		if (const auto ck = req.header("cookie"); !ck.empty()) {
			std::istringstream cs(ck);
			std::string kv;
			while (std::getline(cs, kv, ';')) {
				kv = trim(kv);
				const auto eq = kv.find('=');
				if (eq != std::string::npos)
					req.cookies[kv.substr(0, eq)] = kv.substr(eq + 1);
			}
		}
		// Body.
		const size_t len = std::strtoul(req.header("content-length").c_str(), nullptr, 10);
		if (len > kMaxBody) {
			c.write("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
			return;
		}
		while (c.buf.size() < len)
			if (!c.fill())
				return;
		req.body = c.buf.substr(0, len);
		c.buf.erase(0, len);

		Response resp;
		try {
			resp = handler_(req);
		} catch (const std::exception &e) {
			resp = Response::text(std::string("internal error: ") + e.what() + "\n", 500);
		}
		const bool close = lower(req.header("connection")) == "close" || version == "HTTP/1.0";
		std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n";
		bool has_type = false;
		for (const auto &[k, v] : resp.headers) {
			out += k + ": " + v + "\r\n";
			if (lower(k) == "content-type")
				has_type = true;
		}
		if (!has_type && !resp.body.empty())
			out += "Content-Type: text/plain; charset=utf-8\r\n";
		out += "Content-Length: " + std::to_string(req.method == "HEAD" ? 0 : resp.body.size()) + "\r\n";
		if (close)
			out += "Connection: close\r\n";
		out += "\r\n";
		if (req.method != "HEAD")
			out += resp.body;
		if (!c.write(out) || close)
			return;
	}
}

} // namespace xst::http
