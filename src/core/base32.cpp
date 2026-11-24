#include "core/base32.hpp"

namespace xst::base32 {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

int value_of(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= '2' && c <= '7')
		return c - '2' + 26;
	return -1;
}
} // namespace

std::string encode(std::span<const uint8_t> data)
{
	std::string out;
	out.reserve((data.size() * 8 + 4) / 5);
	uint32_t buffer = 0;
	int bits = 0;
	for (uint8_t byte : data) {
		buffer = buffer << 8 | byte;
		bits += 8;
		while (bits >= 5) {
			out.push_back(kAlphabet[(buffer >> (bits - 5)) & 31]);
			bits -= 5;
		}
	}
	if (bits > 0)
		out.push_back(kAlphabet[(buffer << (5 - bits)) & 31]);
	return out;
}

std::optional<std::vector<uint8_t>> decode(std::string_view text)
{
	while (!text.empty() && text.back() == '=')
		text.remove_suffix(1);
	std::vector<uint8_t> out;
	out.reserve(text.size() * 5 / 8);
	uint32_t buffer = 0;
	int bits = 0;
	for (char c : text) {
		const int v = value_of(c);
		if (v < 0)
			return std::nullopt;
		buffer = buffer << 5 | static_cast<uint32_t>(v);
		bits += 5;
		if (bits >= 8) {
			out.push_back(static_cast<uint8_t>(buffer >> (bits - 8)));
			bits -= 8;
		}
	}
	return out;
}

} // namespace xst::base32
