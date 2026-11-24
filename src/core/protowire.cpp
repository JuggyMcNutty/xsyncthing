#include "core/protowire.hpp"

namespace xst::protowire {

void Writer::raw_varint(uint64_t value)
{
	while (value >= 0x80) {
		out_.push_back(static_cast<uint8_t>(value | 0x80));
		value >>= 7;
	}
	out_.push_back(static_cast<uint8_t>(value));
}

void Writer::varint(uint32_t field, uint64_t value)
{
	raw_varint(static_cast<uint64_t>(field) << 3 | kVarint);
	raw_varint(value);
}

void Writer::bytes(uint32_t field, std::span<const uint8_t> value)
{
	raw_varint(static_cast<uint64_t>(field) << 3 | kLen);
	raw_varint(value.size());
	out_.insert(out_.end(), value.begin(), value.end());
}

void Writer::string(uint32_t field, std::string_view value)
{
	bytes(field, std::span(reinterpret_cast<const uint8_t *>(value.data()), value.size()));
}

std::optional<uint64_t> Reader::raw_varint()
{
	uint64_t value = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (pos_ >= data_.size())
			return std::nullopt;
		const uint8_t b = data_[pos_++];
		value |= static_cast<uint64_t>(b & 0x7F) << shift;
		if (!(b & 0x80))
			return value;
	}
	return std::nullopt;
}

std::optional<Field> Reader::next()
{
	if (error_ || pos_ >= data_.size())
		return std::nullopt;
	const auto key = raw_varint();
	if (!key || (*key >> 3) == 0) {
		error_ = true;
		return std::nullopt;
	}
	Field f;
	f.number = static_cast<uint32_t>(*key >> 3);
	f.type = static_cast<WireType>(*key & 7);
	switch (f.type) {
	case kVarint: {
		const auto v = raw_varint();
		if (!v) {
			error_ = true;
			return std::nullopt;
		}
		f.varint = *v;
		break;
	}
	case kFixed64:
	case kFixed32: {
		const size_t n = f.type == kFixed64 ? 8 : 4;
		if (data_.size() - pos_ < n) {
			error_ = true;
			return std::nullopt;
		}
		for (size_t i = 0; i < n; i++)
			f.varint |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i); // little-endian on the wire
		pos_ += n;
		break;
	}
	case kLen: {
		const auto n = raw_varint();
		if (!n || *n > data_.size() - pos_) {
			error_ = true;
			return std::nullopt;
		}
		f.bytes = data_.subspan(pos_, static_cast<size_t>(*n));
		pos_ += static_cast<size_t>(*n);
		break;
	}
	default:
		error_ = true;
		return std::nullopt;
	}
	return f;
}

} // namespace xst::protowire
