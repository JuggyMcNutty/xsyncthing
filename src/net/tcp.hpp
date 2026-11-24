#pragma once

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
	int fd() const { return fd_; }

	// Connects to host:port (a dotted IPv4 address or a name); nullptr and error on failure.
	static std::unique_ptr<TcpStream> connect(const std::string &host, uint16_t port, std::string *error = nullptr);

private:
	int fd_ = -1;
};

// A listening IPv4 TCP socket.
class TcpListener {
public:
	~TcpListener();
	static std::unique_ptr<TcpListener> listen(uint16_t port, std::string *error = nullptr);
	std::unique_ptr<TcpStream> accept(std::string *peer = nullptr);
	uint16_t port() const { return port_; }
	void close();
	// Wakes a blocked accept() (which then returns nullptr).
	void shutdown();

private:
	int fd_ = -1;
	uint16_t port_ = 0;
};

} // namespace xst
