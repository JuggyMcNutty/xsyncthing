#include "net/addresses.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

extern "C" {
#include "xsdk/net.h"
}

namespace xst {

std::vector<std::string> lan_ipv4_addresses()
{
	in_addr a{};
	if (xsdk_net_address(&a) != 0)
		return {};
	char text[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &a, text, sizeof text);
	return { text };
}

} // namespace xst
