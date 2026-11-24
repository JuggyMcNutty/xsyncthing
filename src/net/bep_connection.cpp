#include "net/bep_connection.hpp"

#include <array>
#include <vector>

namespace xst {

bool BepConnection::send(const bep::Message &message)
{
	const auto frame = bep::frame(message, policy_);
	std::lock_guard lock(send_mutex_);
	return tls_->write_all(frame);
}

std::optional<bep::Message> BepConnection::receive(std::string *error)
{
	auto fail = [&](const std::string &what) -> std::optional<bep::Message> {
		if (error)
			*error = what;
		return std::nullopt;
	};
	std::array<uint8_t, 4> buf{};
	if (!tls_->read_full(std::span(buf).first(2)))
		return fail("reading header length: EOF");
	const uint16_t header_length = static_cast<uint16_t>(buf[0] << 8 | buf[1]);
	if (header_length > 0x7FFF)
		return fail("negative header length");
	std::vector<uint8_t> header_bytes(header_length);
	if (!tls_->read_full(header_bytes))
		return fail("reading header: EOF");
	const auto header = bep::decode_header(header_bytes);
	if (!header)
		return fail("malformed header");
	if (!tls_->read_full(buf))
		return fail("reading message length: EOF");
	const uint32_t length = static_cast<uint32_t>(buf[0]) << 24 | static_cast<uint32_t>(buf[1]) << 16 |
		static_cast<uint32_t>(buf[2]) << 8 | buf[3];
	if (length > bep::kMaxMessageLen)
		return fail("message length " + std::to_string(length) + " exceeds the maximum");
	std::vector<uint8_t> body(length);
	if (!tls_->read_full(body))
		return fail("reading message: EOF");
	return bep::unframe(*header, body, error);
}

void BepConnection::close(const std::string &reason)
{
	send(bep::Close{ reason });
	tls_->close();
}

} // namespace xst
