#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/bep.hpp"

namespace xst::bep {

// How a connection compresses what it sends (the device's "compression" setting).
enum class CompressionPolicy { metadata, never, always };

// One message on the wire after Hello:
//   u16 header length | Header | u32 message length | message (LZ4 block prefixed by u32 uncompressed size if LZ4)
// all big-endian. Compression is used only if it saves at least 1/32 of the size, as upstream does.
std::vector<uint8_t> frame(const Message &message, CompressionPolicy policy);

// LZ4 as BEP uses it: a u32 big-endian uncompressed size, then one LZ4 block.
std::optional<std::vector<uint8_t>> lz4_compress(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> lz4_decompress(std::span<const uint8_t> data);

// Decodes a message body given its header (decompressing it first if needed).
std::optional<Message> unframe(const Header &header, std::span<const uint8_t> body, std::string *error = nullptr);

} // namespace xst::bep
