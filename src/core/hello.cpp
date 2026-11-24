#include "core/hello.hpp"

#include "core/protowire.hpp"

namespace xst {

std::vector<uint8_t> Hello::encode_frame() const
{
	protowire::Writer w;
	w.string_nz(1, device_name);
	w.string_nz(2, client_name);
	w.string_nz(3, client_version);
	w.varint_nz(4, static_cast<uint64_t>(static_cast<int64_t>(num_connections)));
	w.varint_nz(5, static_cast<uint64_t>(timestamp));
	const auto &msg = w.data();
	std::vector<uint8_t> frame = {
		static_cast<uint8_t>(kMagic >> 24),
		static_cast<uint8_t>(kMagic >> 16),
		static_cast<uint8_t>(kMagic >> 8),
		static_cast<uint8_t>(kMagic),
		static_cast<uint8_t>(msg.size() >> 8),
		static_cast<uint8_t>(msg.size()),
	};
	frame.insert(frame.end(), msg.begin(), msg.end());
	return frame;
}

std::optional<Hello> Hello::decode(std::span<const uint8_t> message)
{
	Hello h;
	protowire::Reader r(message);
	while (auto f = r.next()) {
		switch (f->number) {
		case 1:
			if (f->type == protowire::kLen)
				h.device_name = f->str();
			break;
		case 2:
			if (f->type == protowire::kLen)
				h.client_name = f->str();
			break;
		case 3:
			if (f->type == protowire::kLen)
				h.client_version = f->str();
			break;
		case 4:
			if (f->type == protowire::kVarint)
				h.num_connections = static_cast<int32_t>(f->varint);
			break;
		case 5:
			if (f->type == protowire::kVarint)
				h.timestamp = static_cast<int64_t>(f->varint);
			break;
		default:
			break; // unknown fields are skipped, as protobuf does
		}
	}
	if (r.error())
		return std::nullopt;
	// Upstream validates these (bep.proto: max_len 1024, num_connections >= 0).
	if (h.device_name.size() > 1024 || h.client_name.size() > 1024 || h.client_version.size() > 1024 ||
		h.num_connections < 0)
		return std::nullopt;
	return h;
}

HelloHeader parse_hello_header(std::span<const uint8_t, 6> header, uint16_t *length)
{
	const uint32_t magic = static_cast<uint32_t>(header[0]) << 24 | static_cast<uint32_t>(header[1]) << 16 |
		static_cast<uint32_t>(header[2]) << 8 | header[3];
	if (magic == Hello::kMagic) {
		*length = static_cast<uint16_t>(header[4] << 8 | header[5]);
		return *length > Hello::kMaxSize ? HelloHeader::unknown_magic : HelloHeader::ok;
	}
	// The start of an old ClusterConfig, or the v0.13 magic.
	if (magic == 0x00010001 || magic == 0x00010000 || magic == 0x9F79BC40)
		return HelloHeader::too_old;
	return HelloHeader::unknown_magic;
}

} // namespace xst
