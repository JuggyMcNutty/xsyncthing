// The optional services, registered on the App. Each phase adds its own here.
#include "app.hpp"
#include "net/addresses.hpp"
#include "net/global_discovery.hpp"
#include "net/local_discovery.hpp"

namespace xst {

void register_services(App &app)
{
	app.on_core_ready([](App &a) {
		auto local = std::make_unique<LocalDiscovery>(a.config(), a.events(), a.identity().id,
			[&a] { return a.connections().listen_port(); });
		a.connections().add_address_source(local.get());
		a.set_local_discovery(local.get());
		a.add_service(std::move(local));

		// What we tell global discovery: the listen port with the host unspecified (the server substitutes the
		// address it sees us at), and our LAN addresses when announceLANAddresses is on (the default).
		auto global = std::make_unique<GlobalDiscovery>(a.config(), a.identity(), [&a] {
			std::vector<std::string> out;
			const uint16_t port = a.connections().listen_port();
			if (port == 0)
				return out;
			out.push_back("tcp://0.0.0.0:" + std::to_string(port));
			if (a.config().options().value("announceLANAddresses", true))
				for (const auto &ip : lan_ipv4_addresses())
					out.push_back("tcp://" + ip + ":" + std::to_string(port));
			for (const auto &extra : a.extra_announce_addresses())
				out.push_back(extra);
			return out;
		});
		a.connections().add_address_source(global.get());
		a.set_global_discovery(global.get());
		a.add_service(std::move(global));
	});
}

} // namespace xst
