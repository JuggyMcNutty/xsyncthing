#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "net/tls.hpp"

namespace xst {

// A TCP connection as a Stream; IPv4 (the console has no IPv6).
class TcpStream : public Stream {
public:
	explicit TcpStream(int fd) : fd_(fd) {}
	~TcpStream() override { close(); }
	long read(std::span<uint8_t> buffer) override;
	long write(std::span<const uint8_t> data) override;
	void close() override;
	void interrupt() override;
	// Bounds each send and receive (SO_SNDTIMEO / SO_RCVTIMEO); a read that times out fails.
	void set_timeout(int timeout_ms);
	int fd() const { return fd_; }

	// Connects to host:port (a dotted IPv4 address or a name) within timeout_ms; nullptr and error on failure.
	static std::unique_ptr<TcpStream> connect(const std::string &host, uint16_t port, std::string *error = nullptr,
		int timeout_ms = 10000);

private:
	std::atomic<int> fd_{ -1 };
};

// A listening IPv4 TCP socket.
class TcpListener {
public:
	~TcpListener();
	static std::unique_ptr<TcpListener> listen(uint16_t port, std::string *error = nullptr);
	// Waits up to timeout_ms for a connection; nullptr if none came (or the listener is closed). Bounded, because
	// on the console (Winsock under XAM) shutdown() doesn't wake a blocked accept().
	std::unique_ptr<TcpStream> accept(std::string *peer = nullptr, int timeout_ms = 500);
	uint16_t port() const { return port_; }
	void close();
	// Wakes a blocked accept() (which then returns nullptr).
	void shutdown();

private:
	int fd_ = -1;
	uint16_t port_ = 0;
};

} // namespace xst
