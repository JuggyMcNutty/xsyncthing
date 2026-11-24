#include "core/version.hpp"
#include "test.hpp"

TEST(client_name_is_xsyncthing) { CHECK_EQ(xst::client_name(), "xsyncthing"); }
TEST(client_version_starts_with_v) { CHECK(xst::client_version().starts_with("v")); }
