#pragma once

#include <map>
#include <optional>
#include <string>

#include "net/identity.hpp"

namespace xst {

// A minimal HTTP/1.1 client over our TLS stack (or plain TCP), for global discovery and the relay pool. One request
// per connection (Connection: close). Handles Content-Length, chunked bodies and reading to EOF.
struct HttpResponse {
	int status = 0;
	std::string reason;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;
	std::string header(const std::string &name) const
	{
		auto it = headers.find(name);
		return it == headers.end() ? "" : it->second;
	}
	// The server's certificate (DER) for https, to check a pinned device ID.
	std::vector<uint8_t> peer_cert;
};

struct HttpRequest {
	std::string method = "GET";
	std::string url; // http:// or https://
	std::map<std::string, std::string> headers;
	std::string body;
	// https: verify against these roots (empty = don't verify, e.g. when a device ID is pinned instead).
	std::string ca_pem;
	// https: present this client certificate (global discovery announces do).
	const Identity *client_identity = nullptr;
	int timeout_ms = 30000;
};

std::optional<HttpResponse> http_request(const HttpRequest &request, std::string *error = nullptr);

// Splits "https://host:port/path?query" into its parts; false if not http(s).
struct Url {
	std::string scheme, host, path_and_query, query;
	uint16_t port = 0;
	std::map<std::string, std::string> params; // query parameters, decoded
};
std::optional<Url> parse_url(const std::string &url);
std::string url_encode(const std::string &s);

// The CA bundle shipped beside the binary (assets/cacert.pem); empty if missing.
const std::string &ca_bundle();

} // namespace xst
