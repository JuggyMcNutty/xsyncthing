#pragma once

#include <string>
#include <vector>

namespace xst {

// This machine's IPv4 addresses on the LAN (not loopback), as text. The console has one: xsdk_net_address.
std::vector<std::string> lan_ipv4_addresses();

} // namespace xst
