// The optional services, registered on the App. Each phase adds its own here.
#include "app.hpp"
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
	});
}

} // namespace xst
