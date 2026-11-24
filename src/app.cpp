#include "app.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "platform/platform.hpp"

namespace xst {

namespace {

std::string read_file(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

bool write_file(const std::string &path, const std::string &data)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << data;
	out.close();
	return static_cast<bool>(out);
}

void mkdir_p(const std::string &path)
{
	for (size_t i = 1; i <= path.size(); i++)
		if (i == path.size() || path[i] == '/')
			::mkdir(path.substr(0, i).c_str(), 0755);
}

} // namespace

uint64_t short_id_of(const DeviceId &id) { return id.short_id(); }

App::App(Options options) : options_(std::move(options)) {}

App::~App() { stop(); }

bool App::start(std::string *error)
{
	mkdir_p(options_.data_dir);
	const std::string cert = options_.data_dir + "/cert.pem";
	const std::string key = options_.data_dir + "/key.pem";
	const std::string cert_pem = read_file(cert);
	const std::string key_pem = read_file(key);
	if (!cert_pem.empty() && !key_pem.empty()) {
		identity_ = Identity::from_pem(cert_pem, key_pem, error);
		if (!identity_)
			return false;
	} else {
		identity_ = Identity::generate(error);
		if (!identity_)
			return false;
		write_file(cert, identity_->cert_pem());
		write_file(key, identity_->key_pem());
		platform::log("app: generated a new identity");
	}
	platform::log("app: device ID " + identity_->id.to_string());

	if (!config_.load(options_.data_dir + "/config.json", identity_->id, options_.device_name, error))
		return false;
	db_ = Db::open(options_.data_dir + "/index.db", error);
	if (!db_)
		return false;
	model_ = std::make_unique<Model>(config_, *db_, events_, options_.open_fs, short_id_of(identity_->id));
	connections_ = std::make_unique<ConnectionService>(*model_, config_, *identity_, options_.device_name);
	for (auto &fn : core_ready_)
		fn(*this);
	model_->start();
	connections_->start();
	for (auto &s : services_)
		s->start();
	started_ = true;
	return true;
}

void App::stop()
{
	if (!started_)
		return;
	started_ = false;
	for (auto it = services_.rbegin(); it != services_.rend(); ++it)
		(*it)->stop();
	if (connections_)
		connections_->stop();
	if (model_)
		model_->stop();
}

} // namespace xst
