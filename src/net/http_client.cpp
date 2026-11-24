#include "net/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "net/tcp.hpp"
#include "net/tls.hpp"

namespace xst {

namespace {

std::string lower(std::string s)
{
	for (auto &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string url_decode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
			i += 2;
		} else if (s[i] == '+') {
			out.push_back(' ');
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

// Reads bytes from a TLS connection or a plain stream.
struct Reader {
	TlsConnection *tls = nullptr;
	Stream *plain = nullptr;
	std::string buf;
	bool eof = false;
	bool fill()
	{
		if (eof)
			return false;
		uint8_t chunk[8192];
		const long n = tls ? tls->read(chunk) : plain->read(chunk);
		if (n <= 0) {
			eof = true;
			return false;
		}
		buf.append(reinterpret_cast<char *>(chunk), static_cast<size_t>(n));
		return true;
	}
	std::optional<std::string> line()
	{
		for (;;) {
			const auto nl = buf.find("\r\n");
			if (nl != std::string::npos) {
				std::string l = buf.substr(0, nl);
				buf.erase(0, nl + 2);
				return l;
			}
			if (buf.size() > 64 * 1024 || !fill())
				return std::nullopt;
		}
	}
	bool exactly(size_t n, std::string &out)
	{
		while (buf.size() < n)
			if (!fill())
				return false;
		out.append(buf, 0, n);
		buf.erase(0, n);
		return true;
	}
	void rest(std::string &out)
	{
		while (fill()) {
		}
		out += buf;
		buf.clear();
	}
};

} // namespace

std::string url_encode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out.push_back(static_cast<char>(c));
		else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}

std::optional<Url> parse_url(const std::string &url)
{
	Url u;
	const auto se = url.find("://");
	if (se == std::string::npos)
		return std::nullopt;
	u.scheme = lower(url.substr(0, se));
	if (u.scheme != "http" && u.scheme != "https")
		return std::nullopt;
	std::string rest = url.substr(se + 3);
	const auto slash = rest.find_first_of("/?");
	std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
	u.path_and_query = slash == std::string::npos ? "/" : rest.substr(slash);
	if (u.path_and_query.starts_with("?"))
		u.path_and_query = "/" + u.path_and_query;
	const auto colon = hostport.rfind(':');
	if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
		u.host = hostport.substr(0, colon);
		u.port = static_cast<uint16_t>(std::atoi(hostport.c_str() + colon + 1));
	} else {
		u.host = hostport;
		u.port = u.scheme == "https" ? 443 : 80;
	}
	const auto q = u.path_and_query.find('?');
	if (q != std::string::npos) {
		u.query = u.path_and_query.substr(q + 1);
		std::stringstream ss(u.query);
		std::string kv;
		while (std::getline(ss, kv, '&')) {
			if (kv.empty())
				continue;
			const auto eq = kv.find('=');
			u.params[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
		}
	}
	return u.host.empty() ? std::nullopt : std::optional(u);
}

const std::string &ca_bundle()
{
	static const std::string bundle = [] {
		std::vector<std::string> candidates;
		if (const char *a = std::getenv("XST_ASSETS"))
			candidates.push_back(std::string(a) + "/cacert.pem");
#if defined(__XSDK__)
		candidates.push_back("game:/cacert.pem");
#else
		candidates.push_back("assets/cacert.pem");
		candidates.push_back(XST_SOURCE_DIR "/assets/cacert.pem");
#endif
		for (const auto &c : candidates) {
			std::ifstream in(c, std::ios::binary);
			if (in) {
				std::stringstream ss;
				ss << in.rdbuf();
				return ss.str();
			}
		}
		return std::string();
	}();
	return bundle;
}

std::optional<HttpResponse> http_request(const HttpRequest &req, std::string *error)
{
	auto fail = [&](const std::string &why) -> std::optional<HttpResponse> {
		if (error)
			*error = why;
		return std::nullopt;
	};
	const auto u = parse_url(req.url);
	if (!u)
		return fail("bad URL " + req.url);
	std::string e;
	auto tcp = TcpStream::connect(u->host, u->port, &e, req.timeout_ms);
	if (!tcp)
		return fail(e);
	// Bound each read and write so a stalled server can't hold a thread forever.
	tcp->set_timeout(req.timeout_ms);

	std::unique_ptr<TlsConnection> tls;
	std::unique_ptr<Stream> plain;
	if (u->scheme == "https") {
		TlsOptions o;
		o.role = TlsOptions::Role::client;
		o.min_version = 12;
		o.identity = req.client_identity;
		o.require_peer_cert = false;
		o.ca_pem = req.ca_pem;
		o.server_name = u->host;
		tls = TlsConnection::handshake(std::move(tcp), o, &e);
		if (!tls)
			return fail(e);
	} else {
		plain = std::move(tcp);
	}

	std::string head = req.method + " " + u->path_and_query + " HTTP/1.1\r\nHost: " + u->host;
	if ((u->scheme == "https" && u->port != 443) || (u->scheme == "http" && u->port != 80))
		head += ":" + std::to_string(u->port);
	head += "\r\nConnection: close\r\nUser-Agent: xsyncthing\r\nAccept: */*\r\n";
	for (const auto &[k, v] : req.headers)
		head += k + ": " + v + "\r\n";
	if (!req.body.empty() || req.method == "POST" || req.method == "PUT")
		head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
	head += "\r\n";
	const std::string all = head + req.body;
	const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(all.data()), all.size());
	if (tls ? !tls->write_all(bytes) : plain->write(bytes) != static_cast<long>(bytes.size()))
		return fail("sending the request failed");

	Reader r;
	r.tls = tls.get();
	r.plain = plain.get();
	HttpResponse resp;
	if (tls)
		resp.peer_cert = tls->peer_cert();
	const auto status = r.line();
	if (!status || !status->starts_with("HTTP/1."))
		return fail("no HTTP response");
	const auto sp1 = status->find(' ');
	resp.status = std::atoi(status->c_str() + sp1 + 1);
	const auto sp2 = status->find(' ', sp1 + 1);
	resp.reason = sp2 == std::string::npos ? "" : status->substr(sp2 + 1);
	for (;;) {
		const auto l = r.line();
		if (!l)
			return fail("truncated headers");
		if (l->empty())
			break;
		const auto colon = l->find(':');
		if (colon == std::string::npos)
			continue;
		std::string v = l->substr(colon + 1);
		while (!v.empty() && v.front() == ' ')
			v.erase(0, 1);
		resp.headers[lower(l->substr(0, colon))] = v;
	}
	if (req.method == "HEAD" || resp.status == 204 || resp.status == 304)
		return resp;
	if (lower(resp.header("transfer-encoding")).find("chunked") != std::string::npos) {
		for (;;) {
			const auto size_line = r.line();
			if (!size_line)
				return fail("truncated chunked body");
			const size_t n = std::strtoul(size_line->c_str(), nullptr, 16);
			if (n == 0) {
				while (auto t = r.line())
					if (t->empty())
						break;
				break;
			}
			if (!r.exactly(n, resp.body))
				return fail("truncated chunk");
			r.line();
		}
	} else if (!resp.header("content-length").empty()) {
		const size_t n = std::strtoul(resp.header("content-length").c_str(), nullptr, 10);
		if (n > 16 * 1024 * 1024)
			return fail("response too large");
		if (!r.exactly(n, resp.body))
			return fail("truncated body");
	} else {
		r.rest(resp.body);
	}
	return resp;
}

} // namespace xst
