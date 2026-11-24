#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/bep.hpp"
#include "core/bep_frame.hpp"
#include "net/tls.hpp"

namespace xst {

// A BEP connection after Hello: sends and receives framed messages over TLS. send() may be called from several
// threads; receive() from one reader thread.
class BepConnection {
public:
	BepConnection(std::unique_ptr<TlsConnection> tls, bep::CompressionPolicy policy)
		: tls_(std::move(tls)), policy_(policy)
	{
	}

	bool send(const bep::Message &message);
	// The next message; nullopt when the connection ends or a message is malformed (error says which).
	std::optional<bep::Message> receive(std::string *error = nullptr);
	void close(const std::string &reason);

	TlsConnection &tls() { return *tls_; }
	DeviceId peer() const { return tls_->peer_device_id(); }

private:
	std::unique_ptr<TlsConnection> tls_;
	bep::CompressionPolicy policy_;
	std::mutex send_mutex_;
};

} // namespace xst
