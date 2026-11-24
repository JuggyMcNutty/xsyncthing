#include "net/bep_handshake.hpp"

#include <array>
#include <vector>

namespace xst {

TlsOptions bep_tls_options(const Identity &identity, TlsOptions::Role role)
{
	TlsOptions options;
	options.role = role;
	options.min_version = 13;
	options.alpn = { kBepAlpn };
	options.identity = &identity;
	options.require_peer_cert = true;
	return options;
}

std::optional<Hello> exchange_hello(TlsConnection &conn, const Hello &ours, std::string *error)
{
	auto fail = [&](const std::string &what) -> std::optional<Hello> {
		if (error)
			*error = what;
		return std::nullopt;
	};
	const auto frame = ours.encode_frame();
	if (!conn.write_all(frame))
		return fail("writing Hello failed");

	std::array<uint8_t, 6> header{};
	if (!conn.read_full(header))
		return fail("reading Hello failed: the connection closed");
	uint16_t length = 0;
	switch (parse_hello_header(header, &length)) {
	case HelloHeader::too_old:
		return fail("the remote device speaks an older version of the protocol");
	case HelloHeader::unknown_magic:
		return fail("the remote device speaks an unknown protocol");
	case HelloHeader::ok:
		break;
	}
	std::vector<uint8_t> message(length);
	if (!conn.read_full(message))
		return fail("reading Hello's body failed");
	auto hello = Hello::decode(message);
	if (!hello)
		return fail("malformed Hello");
	return hello;
}

} // namespace xst
