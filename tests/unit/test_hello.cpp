#include "core/hello.hpp"
#include "test.hpp"

TEST(hello_round_trip)
{
	xst::Hello h;
	h.device_name = "xbox";
	h.client_name = "xsyncthing";
	h.client_version = "v0.0.1";
	h.num_connections = 1;
	h.timestamp = 1700000000123456789;
	const auto frame = h.encode_frame();
	CHECK_EQ(frame[0], 0x2E);
	CHECK_EQ(frame[3], 0x0B);
	uint16_t length = 0;
	CHECK(xst::parse_hello_header(std::span<const uint8_t, 6>(frame.data(), 6), &length) == xst::HelloHeader::ok);
	CHECK_EQ(length, frame.size() - 6);
	const auto back = xst::Hello::decode(std::span(frame).subspan(6));
	CHECK(back.has_value());
	CHECK_EQ(back->device_name, "xbox");
	CHECK_EQ(back->client_version, "v0.0.1");
	CHECK_EQ(back->num_connections, 1);
	CHECK_EQ(back->timestamp, 1700000000123456789);
}

TEST(hello_old_magic)
{
	const uint8_t header[6] = { 0x00, 0x01, 0x00, 0x01, 0, 0 };
	uint16_t length = 0;
	CHECK(xst::parse_hello_header(header, &length) == xst::HelloHeader::too_old);
}
