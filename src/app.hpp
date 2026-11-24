#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/db.hpp"
#include "core/events.hpp"
#include "core/model.hpp"
#include "net/connection_service.hpp"
#include "net/identity.hpp"

namespace xst {

// A service started after the core and stopped before it (discovery, relays, the REST API, ...).
class Service {
public:
	virtual ~Service() = default;
	virtual void start() = 0;
	virtual void stop() = 0;
};

// Everything xsyncthing runs, assembled the same way on the host and the console. The platform supplies the data
// directory, the folder filesystem factory, and the device name.
class App {
public:
	struct Options {
		std::string data_dir;
		std::string device_name;
		FilesystemFactory open_fs;
		std::string gui_assets_dir; // the stock Web GUI's files; empty disables the GUI
		bool enable_api = true;
	};

	explicit App(Options options);
	~App();

	bool start(std::string *error);
	void stop();

	const Identity &identity() const { return *identity_; }
	Config &config() { return config_; }
	Model &model() { return *model_; }
	Events &events() { return events_; }
	ConnectionService &connections() { return *connections_; }
	const Options &options() const { return options_; }
	// Adds a service; started with start(), stopped in reverse order with stop(). Call before start().
	void add_service(std::unique_ptr<Service> service) { services_.push_back(std::move(service)); }
	// Builds services that need the core (called inside start(), after the model exists).
	void on_core_ready(std::function<void(App &)> fn) { core_ready_.push_back(std::move(fn)); }

	// Services the API and UI read status from (set by register_services; may be null).
	class LocalDiscovery *local_discovery() const { return local_discovery_; }
	void set_local_discovery(class LocalDiscovery *d) { local_discovery_ = d; }

private:
	Options options_;
	std::optional<Identity> identity_;
	Config config_;
	Events events_;
	std::unique_ptr<Db> db_;
	std::unique_ptr<Model> model_;
	std::unique_ptr<ConnectionService> connections_;
	std::vector<std::unique_ptr<Service>> services_;
	std::vector<std::function<void(App &)>> core_ready_;
	bool started_ = false;
	class LocalDiscovery *local_discovery_ = nullptr;
};

// Adds the optional services (discovery, relays, REST API) the build has; defined in services.cpp.
void register_services(App &app);

// The device's short ID (the first 8 bytes of its ID, big-endian): its version-vector counter ID.
uint64_t short_id_of(const DeviceId &id);

} // namespace xst
