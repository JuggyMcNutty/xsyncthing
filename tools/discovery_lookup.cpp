// xst-lookup: asks global discovery for a device's addresses, through xsyncthing's own client.
//   xst-lookup <device-id> [server-url ...]
#include <cstdio>

#include "core/config.hpp"
#include "net/global_discovery.hpp"
#include "net/identity.hpp"

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <device-id> [server ...]\n", argv[0]);
		return 2;
	}
	const auto id = xst::DeviceId::parse(argv[1]);
	if (!id) {
		std::fprintf(stderr, "not a device ID\n");
		return 2;
	}
	xst::Config cfg;
	std::string error;
	auto identity = xst::Identity::generate(&error);
	cfg.load("/tmp/xst-lookup-config.json", identity->id, "lookup", &error);
	if (argc > 2)
		cfg.modify([&](xst::json &c) {
			c["options"]["globalAnnounceServers"] = xst::json::array();
			for (int i = 2; i < argc; i++)
				c["options"]["globalAnnounceServers"].push_back(argv[i]);
		});
	xst::GlobalDiscovery gd(cfg, *identity, [] { return std::vector<std::string>{}; });
	const auto addrs = gd.lookup(*id);
	for (const auto &a : addrs)
		std::printf("%s\n", a.c_str());
	return addrs.empty() ? 1 : 0;
}
