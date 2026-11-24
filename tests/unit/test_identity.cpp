#include "net/identity.hpp"
#include "test.hpp"

TEST(identity_generate_and_reload)
{
	std::string error;
	const auto id = xst::Identity::generate(&error);
	CHECK(id.has_value());
	CHECK(!id->cert_der.empty());
	CHECK(!id->id.empty());
	const auto again = xst::Identity::from_pem(id->cert_pem(), id->key_pem(), &error);
	CHECK(again.has_value());
	CHECK(again->id == id->id);
	CHECK(again->cert_der == id->cert_der);
}
