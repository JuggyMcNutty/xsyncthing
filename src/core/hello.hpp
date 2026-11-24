#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xst {

// BEP's pre-authentication Hello (proto/bep.proto), sent by both sides straight after the TLS handshake:
// magic 0x2EA7D90B (big-endian u32), length (big-endian u16, at most 32767), then the protobuf message.
struct Hello {
	static constexpr uint32_t kMagic = 0x2EA7D90B;
	static constexpr size_t kMaxSize = 32767;

	std::string device_name;
	std::string client_name;
	std::string client_version;
	int32_t num_connections = 0;
	int64_t timestamp = 0;

	// The whole frame: magic, length, message.
	std::vector<uint8_t> encode_frame() const;
	// The message alone (without the 6-byte frame header).
	static std::optional<Hello> decode(std::span<const uint8_t> message);
};

enum class HelloHeader { ok, too_old, unknown_magic };

// Reads the frame's first 6 bytes: what the magic says and, if ok, the message's length.
HelloHeader parse_hello_header(std::span<const uint8_t, 6> header, uint16_t *length);

} // namespace xst
