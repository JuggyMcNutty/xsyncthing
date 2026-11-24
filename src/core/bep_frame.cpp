#include "core/bep_frame.hpp"

#include <lz4.h>

namespace xst::bep {

namespace {

void put_u16(std::vector<uint8_t> &out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void put_u32(std::vector<uint8_t> &out, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8)
		out.push_back(static_cast<uint8_t>(v >> s));
}

bool should_compress(const Message &message, size_t size, CompressionPolicy policy)
{
	switch (policy) {
	case CompressionPolicy::never:
		return false;
	case CompressionPolicy::always:
		return size >= kCompressionThreshold;
	case CompressionPolicy::metadata:
		return !std::holds_alternative<Response>(message) && size >= kCompressionThreshold;
	}
	return false;
}

} // namespace

std::optional<std::vector<uint8_t>> lz4_compress(std::span<const uint8_t> data)
{
	if (data.size() > static_cast<size_t>(LZ4_MAX_INPUT_SIZE))
		return std::nullopt;
	const int bound = LZ4_compressBound(static_cast<int>(data.size()));
	std::vector<uint8_t> out;
	out.reserve(4 + static_cast<size_t>(bound));
	put_u32(out, static_cast<uint32_t>(data.size()));
	out.resize(4 + static_cast<size_t>(bound));
	const int n = LZ4_compress_default(reinterpret_cast<const char *>(data.data()),
		reinterpret_cast<char *>(out.data() + 4), static_cast<int>(data.size()), bound);
	if (n <= 0)
		return std::nullopt;
	out.resize(4 + static_cast<size_t>(n));
	return out;
}

std::optional<std::vector<uint8_t>> lz4_decompress(std::span<const uint8_t> data)
{
	if (data.size() < 4)
		return std::nullopt;
	const uint32_t size = static_cast<uint32_t>(data[0]) << 24 | static_cast<uint32_t>(data[1]) << 16 |
		static_cast<uint32_t>(data[2]) << 8 | data[3];
	if (size > kMaxMessageLen)
		return std::nullopt;
	std::vector<uint8_t> out(size);
	const int n = LZ4_decompress_safe(reinterpret_cast<const char *>(data.data() + 4),
		reinterpret_cast<char *>(out.data()), static_cast<int>(data.size() - 4), static_cast<int>(size));
	if (n < 0)
		return std::nullopt;
	out.resize(static_cast<size_t>(n));
	return out;
}

std::vector<uint8_t> frame(const Message &message, CompressionPolicy policy)
{
	const Bytes body = encode(message);
	Header header{ type_of(message), MessageCompression::none };
	const Bytes *payload = &body;
	std::optional<std::vector<uint8_t>> compressed;
	if (should_compress(message, body.size(), policy)) {
		compressed = lz4_compress(body);
		// Worth it only if it saves at least 1/32 (lib/protocol writeCompressedMessage).
		if (compressed && compressed->size() <= body.size() - body.size() / 32) {
			header.compression = MessageCompression::lz4;
			payload = &*compressed;
		}
	}
	const Bytes hdr = encode(header);
	std::vector<uint8_t> out;
	out.reserve(2 + hdr.size() + 4 + payload->size());
	put_u16(out, static_cast<uint16_t>(hdr.size()));
	out.insert(out.end(), hdr.begin(), hdr.end());
	put_u32(out, static_cast<uint32_t>(payload->size()));
	out.insert(out.end(), payload->begin(), payload->end());
	return out;
}

std::optional<Message> unframe(const Header &header, std::span<const uint8_t> body, std::string *error)
{
	std::optional<std::vector<uint8_t>> decompressed;
	if (header.compression == MessageCompression::lz4) {
		decompressed = lz4_decompress(body);
		if (!decompressed) {
			if (error)
				*error = "LZ4 decompression failed";
			return std::nullopt;
		}
		body = *decompressed;
	} else if (header.compression != MessageCompression::none) {
		if (error)
			*error = "unknown message compression " + std::to_string(static_cast<int>(header.compression));
		return std::nullopt;
	}
	auto message = decode(header.type, body);
	if (!message && error)
		*error = "malformed message of type " + std::to_string(static_cast<int>(header.type));
	return message;
}

} // namespace xst::bep
