#include "core/bcrypt.hpp"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

namespace xst::bcrypt {

namespace {

#include "core/blowfish_constants.inc"

struct Blowfish {
	uint32_t p[18];
	uint32_t s[4][256];

	uint32_t f(uint32_t x) const
	{
		return ((s[0][x >> 24] + s[1][(x >> 16) & 0xff]) ^ s[2][(x >> 8) & 0xff]) + s[3][x & 0xff];
	}
	void encrypt(uint32_t &l, uint32_t &r) const
	{
		for (int i = 0; i < 16; i += 2) {
			l ^= p[i];
			r ^= f(l);
			r ^= p[i + 1];
			l ^= f(r);
		}
		l ^= p[16];
		r ^= p[17];
		std::swap(l, r);
	}
};

// Reads the next 32 bits of data cyclically (bytes, big-endian).
uint32_t stream_word(const uint8_t *data, size_t len, size_t &pos)
{
	uint32_t w = 0;
	for (int i = 0; i < 4; i++) {
		w = (w << 8) | data[pos];
		pos = (pos + 1) % len;
	}
	return w;
}

void expand_key(Blowfish &bf, const uint8_t *key, size_t key_len, const uint8_t *salt, size_t salt_len)
{
	size_t kp = 0;
	for (auto &x : bf.p)
		x ^= stream_word(key, key_len, kp);
	uint32_t l = 0, r = 0;
	size_t sp = 0;
	for (int i = 0; i < 18; i += 2) {
		if (salt) {
			l ^= stream_word(salt, salt_len, sp);
			r ^= stream_word(salt, salt_len, sp);
		}
		bf.encrypt(l, r);
		bf.p[i] = l;
		bf.p[i + 1] = r;
	}
	for (auto &box : bf.s)
		for (int i = 0; i < 256; i += 2) {
			if (salt) {
				l ^= stream_word(salt, salt_len, sp);
				r ^= stream_word(salt, salt_len, sp);
			}
			bf.encrypt(l, r);
			box[i] = l;
			box[i + 1] = r;
		}
}

const char *kAlphabet = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

int value_of(char c)
{
	const char *p = std::strchr(kAlphabet, c);
	return c && p ? static_cast<int>(p - kAlphabet) : -1;
}

// bcrypt's base64 (its own alphabet, no padding).
std::string encode64(const uint8_t *d, size_t n)
{
	std::string out;
	size_t i = 0;
	while (i < n) {
		uint32_t c1 = d[i++];
		out.push_back(kAlphabet[c1 >> 2]);
		c1 = (c1 & 3) << 4;
		if (i >= n) {
			out.push_back(kAlphabet[c1]);
			break;
		}
		uint32_t c2 = d[i++];
		c1 |= c2 >> 4;
		out.push_back(kAlphabet[c1]);
		c1 = (c2 & 0x0f) << 2;
		if (i >= n) {
			out.push_back(kAlphabet[c1]);
			break;
		}
		c2 = d[i++];
		c1 |= c2 >> 6;
		out.push_back(kAlphabet[c1]);
		out.push_back(kAlphabet[c2 & 0x3f]);
	}
	return out;
}

bool decode64(const std::string &s, uint8_t *out, size_t n)
{
	size_t o = 0, i = 0;
	while (o < n && i + 1 < s.size()) {
		const int c1 = value_of(s[i++]), c2 = value_of(s[i++]);
		if (c1 < 0 || c2 < 0)
			return false;
		out[o++] = static_cast<uint8_t>((c1 << 2) | ((c2 & 0x30) >> 4));
		if (o >= n || i >= s.size())
			break;
		const int c3 = value_of(s[i++]);
		if (c3 < 0)
			return false;
		out[o++] = static_cast<uint8_t>(((c2 & 0x0f) << 4) | ((c3 & 0x3c) >> 2));
		if (o >= n || i >= s.size())
			break;
		const int c4 = value_of(s[i++]);
		if (c4 < 0)
			return false;
		out[o++] = static_cast<uint8_t>(((c3 & 0x03) << 6) | c4);
	}
	return o == n;
}

} // namespace

bool looks_like_hash(const std::string &s)
{
	return s.size() >= 60 && s[0] == '$' && s[1] == '2' && (s[2] == 'a' || s[2] == 'b' || s[2] == 'y') &&
		s[3] == '$' && std::isdigit(static_cast<unsigned char>(s[4])) && std::isdigit(static_cast<unsigned char>(s[5])) &&
		s[6] == '$';
}

std::string hash(const std::string &password, const std::string &setting)
{
	if (setting.size() < 29 || setting[0] != '$' || setting[1] != '2' || setting[3] != '$' || setting[6] != '$')
		return "";
	const char minor = setting[2];
	if (minor != 'a' && minor != 'b' && minor != 'y')
		return "";
	const int cost = (setting[4] - '0') * 10 + (setting[5] - '0');
	if (cost < 4 || cost > 31)
		return "";
	uint8_t salt[16];
	if (!decode64(setting.substr(7, 22), salt, sizeof salt))
		return "";
	// The key: the password with its terminating NUL, at most 72 bytes (as x/crypto/bcrypt).
	std::vector<uint8_t> key(password.begin(), password.end());
	key.push_back(0);
	if (key.size() > 72)
		key.resize(72);

	Blowfish bf;
	std::memcpy(bf.p, kBlowfishP, sizeof bf.p);
	std::memcpy(bf.s, kBlowfishS, sizeof bf.s);
	expand_key(bf, key.data(), key.size(), salt, sizeof salt);
	const uint64_t rounds = uint64_t(1) << cost;
	for (uint64_t i = 0; i < rounds; i++) {
		expand_key(bf, key.data(), key.size(), nullptr, 0);
		expand_key(bf, salt, sizeof salt, nullptr, 0);
	}
	uint32_t c[6];
	const char magic[] = "OrpheanBeholderScryDoubt";
	for (int i = 0; i < 6; i++)
		c[i] = static_cast<uint32_t>(static_cast<uint8_t>(magic[4 * i])) << 24 |
			static_cast<uint32_t>(static_cast<uint8_t>(magic[4 * i + 1])) << 16 |
			static_cast<uint32_t>(static_cast<uint8_t>(magic[4 * i + 2])) << 8 | static_cast<uint8_t>(magic[4 * i + 3]);
	for (int k = 0; k < 64; k++)
		for (int i = 0; i < 6; i += 2)
			bf.encrypt(c[i], c[i + 1]);
	uint8_t out[24];
	for (int i = 0; i < 6; i++) {
		out[4 * i] = static_cast<uint8_t>(c[i] >> 24);
		out[4 * i + 1] = static_cast<uint8_t>(c[i] >> 16);
		out[4 * i + 2] = static_cast<uint8_t>(c[i] >> 8);
		out[4 * i + 3] = static_cast<uint8_t>(c[i]);
	}
	return setting.substr(0, 7) + encode64(salt, 16) + encode64(out, 23);
}

std::string generate(const std::string &password, int cost)
{
	uint8_t salt[16];
	WC_RNG rng;
	wc_InitRng(&rng);
	wc_RNG_GenerateBlock(&rng, salt, sizeof salt);
	wc_FreeRng(&rng);
	char prefix[8];
	std::snprintf(prefix, sizeof prefix, "$2a$%02d$", cost);
	return hash(password, prefix + encode64(salt, 16));
}

bool verify(const std::string &password, const std::string &stored)
{
	if (!looks_like_hash(stored))
		return false;
	const std::string h = hash(password, stored.substr(0, 29));
	if (h.size() != stored.size())
		return false;
	unsigned diff = 0;
	for (size_t i = 0; i < h.size(); i++)
		diff |= static_cast<unsigned>(h[i] ^ stored[i]);
	return diff == 0;
}

} // namespace xst::bcrypt
