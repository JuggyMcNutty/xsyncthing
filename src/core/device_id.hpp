#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xst {

// A Syncthing device ID: the SHA-256 of the device's certificate (DER). As text it is base32, with a check character
// after every 13 characters (Syncthing's base32 variant of Luhn), split into 8 groups of 7 by dashes
// (https://docs.syncthing.net/dev/device-ids.html).
class DeviceId {
public:
	static constexpr size_t kSize = 32;
	using Bytes = std::array<uint8_t, kSize>;

	DeviceId() = default;
	explicit DeviceId(const Bytes &bytes) : bytes_(bytes) {}

	// The ID of the certificate whose DER encoding is der.
	static DeviceId from_certificate(std::span<const uint8_t> der);
	// Parses any form Syncthing accepts: with or without check characters, dashes or spaces, any case, and with
	// 0, 1 and 8 typed for O, I and B. nullopt if it isn't an ID or a check character is wrong.
	static std::optional<DeviceId> parse(std::string_view text);
	static std::optional<DeviceId> from_bytes(std::span<const uint8_t> bytes);

	// "XXXXXXX-XXXXXXX-...", 63 characters; "" for the empty (all-zero) ID.
	std::string to_string() const;
	// The first 7 characters of the ID's first 8 bytes in base32, as Syncthing's logs and GUI show it.
	std::string short_string() const;
	uint64_t short_id() const;

	const Bytes &bytes() const { return bytes_; }
	bool empty() const { return bytes_ == Bytes{}; }

	friend bool operator==(const DeviceId &, const DeviceId &) = default;
	friend auto operator<=>(const DeviceId &, const DeviceId &) = default;

private:
	Bytes bytes_{};
};

// Syncthing's check character for base32 text s; nullopt if s has a character outside base32.
std::optional<char> luhn32(std::string_view s);

} // namespace xst
