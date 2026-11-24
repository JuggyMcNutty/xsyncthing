#include "net/tcp.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace xst {

long TcpStream::read(std::span<uint8_t> buffer)
{
	for (;;) {
		const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
		if (n < 0 && errno == EINTR)
			continue;
		return n;
	}
}

long TcpStream::write(std::span<const uint8_t> data)
{
	for (;;) {
		const ssize_t n = ::send(fd_, data.data(), data.size(), 0);
		if (n < 0 && errno == EINTR)
			continue;
		return n;
	}
}

void TcpStream::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

std::unique_ptr<TcpStream> TcpStream::connect(const std::string &host, uint16_t port, std::string *error)
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *result = nullptr;
		if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
			if (error)
				*error = "cannot resolve " + host;
			return nullptr;
		}
		addr.sin_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr;
		freeaddrinfo(result);
	}
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		if (error)
			*error = std::string("socket: ") + std::strerror(errno);
		return nullptr;
	}
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
		if (error)
			*error = "connect to " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
		::close(fd);
		return nullptr;
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return std::make_unique<TcpStream>(fd);
}

TcpListener::~TcpListener() { close(); }

void TcpListener::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

void TcpListener::shutdown()
{
	if (fd_ >= 0)
		::shutdown(fd_, SHUT_RDWR);
}

std::unique_ptr<TcpListener> TcpListener::listen(uint16_t port, std::string *error)
{
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		if (error)
			*error = std::string("socket: ") + std::strerror(errno);
		return nullptr;
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0) {
		if (error)
			*error = "listen on " + std::to_string(port) + ": " + std::strerror(errno);
		::close(fd);
		return nullptr;
	}
	auto listener = std::make_unique<TcpListener>();
	listener->fd_ = fd;
	socklen_t len = sizeof addr;
	::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
	listener->port_ = ntohs(addr.sin_port);
	return listener;
}

std::unique_ptr<TcpStream> TcpListener::accept(std::string *peer)
{
	sockaddr_in addr{};
	socklen_t len = sizeof addr;
	const int fd = ::accept(fd_, reinterpret_cast<sockaddr *>(&addr), &len);
	if (fd < 0)
		return nullptr;
	if (peer) {
		char text[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
		*peer = std::string(text) + ":" + std::to_string(ntohs(addr.sin_port));
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	return std::make_unique<TcpStream>(fd);
}

} // namespace xst
