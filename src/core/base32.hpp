#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xst::base32 {

// RFC 4648 base32 with the standard alphabet and no padding.
std::string encode(std::span<const uint8_t> data);
// Decodes unpadded (or '='-padded) upper-case base32; nullopt on any other character.
std::optional<std::vector<uint8_t>> decode(std::string_view text);

} // namespace xst::base32
