#include "core/device_id.hpp"

#include <algorithm>
#include <cctype>

#include "core/base32.hpp"
#include "core/sha256.hpp"

namespace xst {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr size_t kGroup = 13;      // base32 characters per check character
constexpr size_t kPlainLength = 52; // 32 bytes in base32, unpadded
constexpr size_t kCheckedLength = 56;

int codepoint(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= '2' && c <= '7')
		return c - '2' + 26;
	return -1;
}
} // namespace

std::optional<char> luhn32(std::string_view s)
{
	// Not quite Luhn: the factor alternates 1, 2 from the first character, and each addend is folded base 32.
	int factor = 1;
	int sum = 0;
	for (char c : s) {
		const int cp = codepoint(c);
		if (cp < 0)
			return std::nullopt;
		int addend = factor * cp;
		factor = factor == 2 ? 1 : 2;
		addend = addend / 32 + addend % 32;
		sum += addend;
	}
	return kAlphabet[(32 - sum % 32) % 32];
}

DeviceId DeviceId::from_certificate(std::span<const uint8_t> der) { return DeviceId(sha256(der)); }

std::optional<DeviceId> DeviceId::from_bytes(std::span<const uint8_t> bytes)
{
	if (bytes.size() != kSize)
		return std::nullopt;
	Bytes b;
	std::copy(bytes.begin(), bytes.end(), b.begin());
	return DeviceId(b);
}

std::optional<DeviceId> DeviceId::parse(std::string_view text)
{
	std::string id;
	id.reserve(text.size());
	for (char c : text) {
		if (c == '-' || c == ' ' || c == '=')
			continue;
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		// What people type for the letters base32 has, as Syncthing accepts.
		if (c == '0')
			c = 'O';
		else if (c == '1')
			c = 'I';
		else if (c == '8')
			c = 'B';
		id.push_back(c);
	}
	if (id.empty())
		return DeviceId();
	if (id.size() == kCheckedLength) {
		std::string plain;
		for (size_t i = 0; i < 4; i++) {
			const std::string_view group(id.data() + i * (kGroup + 1), kGroup);
			const auto check = luhn32(group);
			if (!check || *check != id[i * (kGroup + 1) + kGroup])
				return std::nullopt;
			plain.append(group);
		}
		id = std::move(plain);
	}
	if (id.size() != kPlainLength)
		return std::nullopt;
	const auto bytes = base32::decode(id);
	if (!bytes || bytes->size() != kSize)
		return std::nullopt;
	return from_bytes(*bytes);
}

std::string DeviceId::to_string() const
{
	if (empty())
		return "";
	const std::string plain = base32::encode(bytes_);
	std::string checked;
	for (size_t i = 0; i < 4; i++) {
		const std::string_view group(plain.data() + i * kGroup, kGroup);
		checked.append(group);
		checked.push_back(*luhn32(group));
	}
	std::string out;
	for (size_t i = 0; i < checked.size(); i += 7) {
		if (i > 0)
			out.push_back('-');
		out.append(checked, i, 7);
	}
	return out;
}

uint64_t DeviceId::short_id() const
{
	uint64_t v = 0;
	for (size_t i = 0; i < 8; i++)
		v = v << 8 | bytes_[i];
	return v;
}

std::string DeviceId::short_string() const
{
	if (short_id() == 0)
		return "";
	return base32::encode(std::span(bytes_.data(), 8)).substr(0, 7);
}

} // namespace xst
