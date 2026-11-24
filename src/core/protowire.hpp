#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Protobuf's wire format, by hand, for the few small messages that don't need generated code (Hello, the local
// discovery Announce, Header).
namespace xst::protowire {

enum WireType : uint8_t { kVarint = 0, kFixed64 = 1, kLen = 2, kFixed32 = 5 };

class Writer {
public:
	void varint(uint32_t field, uint64_t value);
	void sint64(uint32_t field, int64_t value) { varint(field, static_cast<uint64_t>(value)); }
	void bytes(uint32_t field, std::span<const uint8_t> value);
	void string(uint32_t field, std::string_view value);
	// Skips proto3 defaults, as the reference encoder does.
	void varint_nz(uint32_t field, uint64_t value)
	{
		if (value)
			varint(field, value);
	}
	void string_nz(uint32_t field, std::string_view value)
	{
		if (!value.empty())
			string(field, value);
	}
	const std::vector<uint8_t> &data() const { return out_; }
	std::vector<uint8_t> take() { return std::move(out_); }

private:
	void raw_varint(uint64_t value);
	std::vector<uint8_t> out_;
};

struct Field {
	uint32_t number = 0;
	WireType type = kVarint;
	uint64_t varint = 0;            // kVarint, kFixed32, kFixed64
	std::span<const uint8_t> bytes; // kLen

	std::string_view str() const { return { reinterpret_cast<const char *>(bytes.data()), bytes.size() }; }
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> data) : data_(data) {}
	// The next field; nullopt at the end. Sets error() on malformed input.
	std::optional<Field> next();
	bool error() const { return error_; }

private:
	std::optional<uint64_t> raw_varint();
	std::span<const uint8_t> data_;
	size_t pos_ = 0;
	bool error_ = false;
};

} // namespace xst::protowire
