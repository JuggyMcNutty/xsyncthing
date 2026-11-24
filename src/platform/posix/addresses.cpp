#include "net/addresses.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace xst {

std::vector<std::string> lan_ipv4_addresses()
{
	std::vector<std::string> out;
	ifaddrs *ifs = nullptr;
	if (getifaddrs(&ifs) != 0)
		return out;
	for (auto *i = ifs; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
			!(i->ifa_flags & IFF_UP))
			continue;
		char text[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, text, sizeof text);
		out.push_back(text);
	}
	freeifaddrs(ifs);
	return out;
}

} // namespace xst
