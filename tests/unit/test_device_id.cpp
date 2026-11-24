// Vectors from upstream syncthing lib/protocol/deviceid_test.go.
#include "core/device_id.hpp"
#include "test.hpp"

using xst::DeviceId;

namespace {
constexpr const char *kFormatted = "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ2";
}

TEST(device_id_parses_all_upstream_forms)
{
	const char *forms[] = {
		"P56IOI-7MZJNU-2IQGDR-EYDM2M-GTMGL3-BXNPQ6-W5BTBB-Z4TJXZ-WICQ",
		"P56IOI-7MZJNU2Y-IQGDR-EYDM2M-GTI-MGL3-BXNPQ6-W5BM-TBB-Z4TJXZ-WICQ2",
		"P56IOI7 MZJNU2I QGDREYD M2MGTMGL 3BXNPQ6W 5BTB BZ4T JXZWICQ",
		"P56IOI7 MZJNU2Y IQGDREY DM2MGTI MGL3BXN PQ6W5BM TBBZ4TJ XZWICQ2",
		"P56IOI7MZJNU2IQGDREYDM2MGTMGL3BXNPQ6W5BTBBZ4TJXZWICQ",
		"p56ioi7mzjnu2iqgdreydm2mgtmgl3bxnpq6w5btbbz4tjxzwicq",
		"P56IOI7MZJNU2YIQGDREYDM2MGTIMGL3BXNPQ6W5BMTBBZ4TJXZWICQ2",
		"P561017MZJNU2YIQGDREYDM2MGTIMGL3BXNPQ6W5BMT88Z4TJXZWICQ2",
		"p56ioi7mzjnu2yiqgdreydm2mgtimgl3bxnpq6w5bmtbbz4tjxzwicq2",
		"p561017mzjnu2yiqgdreydm2mgtimgl3bxnpq6w5bmt88z4tjxzwicq2",
	};
	for (const char *form : forms) {
		const auto id = DeviceId::parse(form);
		CHECK(id.has_value());
		CHECK_EQ(id->to_string(), kFormatted);
	}
}

TEST(device_id_rejects_bad_input)
{
	CHECK(!DeviceId::parse("a"));
	// A wrong check character (the last one changed from 2 to 3).
	CHECK(!DeviceId::parse("P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ3"));
}

TEST(device_id_empty)
{
	const auto id = DeviceId::parse("");
	CHECK(id.has_value());
	CHECK(id->empty());
	CHECK_EQ(id->to_string(), "");
}

TEST(device_id_short)
{
	const auto id = DeviceId::parse(kFormatted);
	CHECK_EQ(id->short_string(), "P56IOI7");
}

TEST(device_id_from_certificate_is_sha256)
{
	// SHA-256("abc") = ba7816bf...
	const uint8_t abc[] = { 'a', 'b', 'c' };
	const auto id = DeviceId::from_certificate(abc);
	CHECK_EQ(id.bytes()[0], 0xba);
	CHECK_EQ(id.bytes()[31], 0xad);
}
