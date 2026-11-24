#include "core/sha256.hpp"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>

namespace xst {

Sha256Digest sha256(std::span<const uint8_t> data)
{
	Sha256Digest digest{};
	wc_Sha256Hash(data.data(), static_cast<word32>(data.size()), digest.data());
	return digest;
}

} // namespace xst
