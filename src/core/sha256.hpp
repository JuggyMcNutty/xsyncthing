#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace xst {

using Sha256Digest = std::array<uint8_t, 32>;

// SHA-256 of data, through wolfCrypt.
Sha256Digest sha256(std::span<const uint8_t> data);

} // namespace xst
