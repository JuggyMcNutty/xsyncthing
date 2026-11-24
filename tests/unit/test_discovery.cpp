#include <fstream>
#include <map>

#include "net/local_discovery.hpp"
#include "test.hpp"

using xst::LocalAnnounce;

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
} // namespace

TEST(local_announce_decodes_upstream_packet)
{
	const auto pkt = golden("local_announce");
	CHECK(!pkt.empty());
	const auto a = LocalAnnounce::decode(pkt);
	CHECK(a.has_value());
	CHECK_EQ(a->id.bytes()[0], 0x40);
	CHECK_EQ(a->addresses.size(), 4u);
	CHECK_EQ(a->instance_id, 1234567890123);
	// Encoding it back gives upstream's bytes exactly.
	CHECK(a->encode() == pkt);
	// Unspecified hosts become the sender's address; QUIC is dropped (no QUIC here); relays pass through.
	const auto r = a->resolved("192.168.1.50");
	CHECK_EQ(r.size(), 3u);
	CHECK_EQ(r[0], "tcp://192.168.1.50:22000");
	CHECK_EQ(r[1], "tcp://192.168.1.50:22000");
	CHECK_EQ(r[2], "relay://1.2.3.4:443/?id=X");
}

TEST(local_announce_rejects_other_magic)
{
	auto pkt = golden("local_announce");
	pkt[0] = 0x7D; // v0.13
	CHECK(!LocalAnnounce::decode(pkt));
}
