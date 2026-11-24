// Relay protocol messages against upstream's XDR encodings (tools/golden/relay.go.txt).
#include <fstream>

#include "net/relay.hpp"
#include "test.hpp"

using namespace xst::relay;

namespace {
std::vector<uint8_t> golden(const std::string &want)
{
	std::ifstream in(XST_GOLDEN_DIR "/bep.txt");
	std::string name, hex;
	while (in >> name >> hex)
		if (name == want) {
			std::vector<uint8_t> b;
			for (size_t i = 0; i + 1 < hex.size(); i += 2)
				b.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
			return b;
		}
	return {};
}
std::vector<uint8_t> seq(size_t n)
{
	std::vector<uint8_t> b(n);
	for (size_t i = 0; i < n; i++)
		b[i] = static_cast<uint8_t>(i + 1);
	return b;
}
uint32_t type_of(const std::vector<uint8_t> &b) { return static_cast<uint32_t>(b[4]) << 24 | b[5] << 16 | b[6] << 8 | b[7]; }
} // namespace

TEST(relay_messages_match_upstream)
{
	CHECK(encode(JoinRelayRequest{ "tok" }) == golden("relay_join"));
	CHECK(encode(ConnectRequest{ seq(32) }) == golden("relay_connect"));
	CHECK(encode(Response{ 1, "not found" }) == golden("relay_response"));
	CHECK(encode(Ping{}) == golden("relay_ping"));
	CHECK(encode(JoinSessionRequest{ seq(16) }) == golden("relay_join_session"));
	SessionInvitation inv{ seq(32), seq(16), { 10, 0, 0, 7 }, 22067, true };
	CHECK(encode(inv) == golden("relay_invitation"));
}

TEST(relay_invitation_decodes)
{
	const auto b = golden("relay_invitation");
	const auto m = decode(type_of(b), std::span(b).subspan(12));
	CHECK(m.has_value());
	const auto &inv = std::get<SessionInvitation>(*m);
	CHECK_EQ(inv.port, 22067);
	CHECK(inv.server_socket);
	CHECK(inv.address == (std::vector<uint8_t>{ 10, 0, 0, 7 }));
	CHECK_EQ(inv.from.size(), 32u);
	const auto r = golden("relay_response");
	const auto rm = decode(type_of(r), std::span(r).subspan(12));
	CHECK(rm && std::get<Response>(*rm).message == "not found");
}
