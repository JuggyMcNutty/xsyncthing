#include "core/bep.hpp"

#include "core/protowire.hpp"

namespace xst::bep {

using protowire::Field;
using protowire::kLen;
using protowire::kVarint;
using protowire::Reader;
using protowire::Writer;

namespace {

// --- helpers -------------------------------------------------------------------------------------------------------

template <class E> uint64_t ev(E e) { return static_cast<uint64_t>(static_cast<int64_t>(e)); }
uint64_t iv(int64_t v) { return static_cast<uint64_t>(v); }

void put_bool(Writer &w, uint32_t field, bool v)
{
	if (v)
		w.varint(field, 1);
}

void put_bytes_nz(Writer &w, uint32_t field, const Bytes &b)
{
	if (!b.empty())
		w.bytes(field, b);
}

void put_message(Writer &w, uint32_t field, const Bytes &encoded) { w.bytes(field, encoded); }

Bytes to_bytes(std::span<const uint8_t> s) { return Bytes(s.begin(), s.end()); }

// Calls fn(field) for each field; false if the encoding is malformed.
template <class Fn> bool each(std::span<const uint8_t> data, Fn &&fn)
{
	Reader r(data);
	while (auto f = r.next())
		if (!fn(*f))
			return false;
	return !r.error();
}

bool is_varint(const Field &f) { return f.type == kVarint; }
bool is_len(const Field &f) { return f.type == kLen; }

// Packed (or, as proto3 also allows, unpacked) repeated int32.
bool read_int32s(const Field &f, std::vector<int32_t> &out)
{
	if (is_varint(f)) {
		out.push_back(static_cast<int32_t>(f.varint));
		return true;
	}
	if (!is_len(f))
		return false;
	// A packed run is a sequence of bare varints.
	size_t pos = 0;
	while (pos < f.bytes.size()) {
		uint64_t v = 0;
		int shift = 0;
		for (;;) {
			if (pos >= f.bytes.size() || shift >= 64)
				return false;
			const uint8_t b = f.bytes[pos++];
			v |= static_cast<uint64_t>(b & 0x7F) << shift;
			shift += 7;
			if (!(b & 0x80))
				break;
		}
		out.push_back(static_cast<int32_t>(v));
	}
	return true;
}

// --- Vector, Counter -----------------------------------------------------------------------------------------------

Bytes enc_counter(const Counter &c)
{
	Writer w;
	w.varint_nz(1, c.id);
	w.varint_nz(2, c.value);
	return w.take();
}

std::optional<Counter> dec_counter(std::span<const uint8_t> data)
{
	Counter c;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_varint(f))
				c.id = f.varint;
			else if (f.number == 2 && is_varint(f))
				c.value = f.varint;
			return true;
		}))
		return std::nullopt;
	return c;
}

// --- BlockInfo -----------------------------------------------------------------------------------------------------

Bytes enc_block(const BlockInfo &b)
{
	Writer w;
	w.varint_nz(1, iv(b.offset));
	w.varint_nz(2, iv(b.size));
	put_bytes_nz(w, 3, b.hash);
	return w.take();
}

std::optional<BlockInfo> dec_block(std::span<const uint8_t> data)
{
	BlockInfo b;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_varint(f))
				b.offset = static_cast<int64_t>(f.varint);
			else if (f.number == 2 && is_varint(f))
				b.size = static_cast<int32_t>(f.varint);
			else if (f.number == 3 && is_len(f))
				b.hash = to_bytes(f.bytes);
			return true;
		}))
		return std::nullopt;
	if (b.offset < 0 || b.size < 0 || (b.hash.size() != 32 && b.hash.size() != 48))
		return std::nullopt;
	return b;
}

// --- FileInfo ------------------------------------------------------------------------------------------------------

Bytes enc_file(const FileInfo &fi, bool with_local)
{
	Writer w;
	w.string_nz(1, fi.name);
	w.varint_nz(2, ev(fi.type));
	w.varint_nz(3, iv(fi.size));
	w.varint_nz(4, fi.permissions);
	w.varint_nz(5, iv(fi.modified_s));
	put_bool(w, 6, fi.deleted);
	put_bool(w, 7, fi.invalid);
	put_bool(w, 8, fi.no_permissions);
	if (!fi.version.counters.empty())
		put_message(w, 9, encode(fi.version));
	w.varint_nz(10, iv(fi.sequence));
	w.varint_nz(11, iv(fi.modified_ns));
	w.varint_nz(12, fi.modified_by);
	w.varint_nz(13, iv(fi.block_size));
	if (!fi.platform.empty())
		put_message(w, 14, fi.platform);
	for (const auto &b : fi.blocks)
		put_message(w, 16, enc_block(b));
	put_bytes_nz(w, 17, fi.symlink_target);
	put_bytes_nz(w, 18, fi.blocks_hash);
	put_bytes_nz(w, 19, fi.encrypted);
	put_bytes_nz(w, 20, fi.previous_blocks_hash);
	if (with_local)
		w.varint_nz(1000, fi.local_flags);
	return w.take();
}

std::optional<FileInfo> dec_file(std::span<const uint8_t> data)
{
	FileInfo fi;
	bool ok = each(data, [&](const Field &f) {
		switch (f.number) {
		case 1:
			if (is_len(f))
				fi.name = f.str();
			break;
		case 2:
			if (is_varint(f))
				fi.type = static_cast<FileInfoType>(f.varint);
			break;
		case 3:
			if (is_varint(f))
				fi.size = static_cast<int64_t>(f.varint);
			break;
		case 4:
			if (is_varint(f))
				fi.permissions = static_cast<uint32_t>(f.varint);
			break;
		case 5:
			if (is_varint(f))
				fi.modified_s = static_cast<int64_t>(f.varint);
			break;
		case 6:
			if (is_varint(f))
				fi.deleted = f.varint != 0;
			break;
		case 7:
			if (is_varint(f))
				fi.invalid = f.varint != 0;
			break;
		case 8:
			if (is_varint(f))
				fi.no_permissions = f.varint != 0;
			break;
		case 9:
			if (is_len(f)) {
				auto v = decode_vector(f.bytes);
				if (!v)
					return false;
				fi.version = std::move(*v);
			}
			break;
		case 10:
			if (is_varint(f))
				fi.sequence = static_cast<int64_t>(f.varint);
			break;
		case 11:
			if (is_varint(f))
				fi.modified_ns = static_cast<int32_t>(f.varint);
			break;
		case 12:
			if (is_varint(f))
				fi.modified_by = f.varint;
			break;
		case 13:
			if (is_varint(f))
				fi.block_size = static_cast<int32_t>(f.varint);
			break;
		case 14:
			if (is_len(f))
				fi.platform = to_bytes(f.bytes);
			break;
		case 16:
			if (is_len(f)) {
				auto b = dec_block(f.bytes);
				if (!b)
					return false;
				fi.blocks.push_back(std::move(*b));
			}
			break;
		case 17:
			if (is_len(f))
				fi.symlink_target = to_bytes(f.bytes);
			break;
		case 18:
			if (is_len(f))
				fi.blocks_hash = to_bytes(f.bytes);
			break;
		case 19:
			if (is_len(f))
				fi.encrypted = to_bytes(f.bytes);
			break;
		case 20:
			if (is_len(f))
				fi.previous_blocks_hash = to_bytes(f.bytes);
			break;
		case 1000:
			if (is_varint(f))
				fi.local_flags = static_cast<uint32_t>(f.varint);
			break;
		default:
			break;
		}
		return true;
	});
	if (!ok)
		return std::nullopt;
	// Upstream's validation (bep.proto).
	if (fi.name.size() > 32767 || fi.size < 0 || fi.sequence < 0 || fi.block_size < 0 ||
		(!fi.blocks_hash.empty() && fi.blocks_hash.size() != 32) ||
		(!fi.previous_blocks_hash.empty() && fi.previous_blocks_hash.size() != 32) ||
		fi.version.counters.size() > 1024)
		return std::nullopt;
	return fi;
}

// --- ClusterConfig -------------------------------------------------------------------------------------------------

Bytes enc_device(const Device &d)
{
	Writer w;
	put_bytes_nz(w, 1, d.id);
	w.string_nz(2, d.name);
	for (const auto &a : d.addresses)
		w.string(3, a);
	w.varint_nz(4, ev(d.compression));
	w.string_nz(5, d.cert_name);
	w.varint_nz(6, iv(d.max_sequence));
	put_bool(w, 7, d.introducer);
	w.varint_nz(8, d.index_id);
	put_bool(w, 9, d.skip_introduction_removals);
	put_bytes_nz(w, 10, d.encryption_password_token);
	return w.take();
}

std::optional<Device> dec_device(std::span<const uint8_t> data)
{
	Device d;
	if (!each(data, [&](const Field &f) {
			switch (f.number) {
			case 1:
				if (is_len(f))
					d.id = to_bytes(f.bytes);
				break;
			case 2:
				if (is_len(f))
					d.name = f.str();
				break;
			case 3:
				if (is_len(f))
					d.addresses.emplace_back(f.str());
				break;
			case 4:
				if (is_varint(f))
					d.compression = static_cast<Compression>(f.varint);
				break;
			case 5:
				if (is_len(f))
					d.cert_name = f.str();
				break;
			case 6:
				if (is_varint(f))
					d.max_sequence = static_cast<int64_t>(f.varint);
				break;
			case 7:
				if (is_varint(f))
					d.introducer = f.varint != 0;
				break;
			case 8:
				if (is_varint(f))
					d.index_id = f.varint;
				break;
			case 9:
				if (is_varint(f))
					d.skip_introduction_removals = f.varint != 0;
				break;
			case 10:
				if (is_len(f))
					d.encryption_password_token = to_bytes(f.bytes);
				break;
			default:
				break;
			}
			return true;
		}))
		return std::nullopt;
	if (d.id.size() != 32 || d.name.size() > 1024 || d.cert_name.size() > 1024 || d.max_sequence < 0)
		return std::nullopt;
	return d;
}

Bytes enc_folder(const Folder &f)
{
	Writer w;
	w.string_nz(1, f.id);
	w.string_nz(2, f.label);
	w.varint_nz(3, ev(f.type));
	w.varint_nz(7, ev(f.stop_reason));
	for (const auto &d : f.devices)
		put_message(w, 16, enc_device(d));
	return w.take();
}

std::optional<Folder> dec_folder(std::span<const uint8_t> data)
{
	Folder folder;
	if (!each(data, [&](const Field &f) {
			switch (f.number) {
			case 1:
				if (is_len(f))
					folder.id = f.str();
				break;
			case 2:
				if (is_len(f))
					folder.label = f.str();
				break;
			case 3:
				if (is_varint(f))
					folder.type = static_cast<FolderType>(f.varint);
				break;
			case 7:
				if (is_varint(f))
					folder.stop_reason = static_cast<FolderStopReason>(f.varint);
				break;
			case 16:
				if (is_len(f)) {
					auto d = dec_device(f.bytes);
					if (!d)
						return false;
					folder.devices.push_back(std::move(*d));
				}
				break;
			default:
				break;
			}
			return true;
		}))
		return std::nullopt;
	if (folder.id.size() > 1024 || folder.label.size() > 1024)
		return std::nullopt;
	return folder;
}

// --- per-message encoders ------------------------------------------------------------------------------------------

Bytes enc(const ClusterConfig &m)
{
	Writer w;
	for (const auto &f : m.folders)
		put_message(w, 1, enc_folder(f));
	put_bool(w, 2, m.secondary);
	return w.take();
}

template <class IndexLike> void enc_index_common(Writer &w, const IndexLike &m)
{
	w.string_nz(1, m.folder);
	for (const auto &f : m.files)
		put_message(w, 2, enc_file(f, false));
	w.varint_nz(3, iv(m.last_sequence));
}

Bytes enc(const Index &m)
{
	Writer w;
	enc_index_common(w, m);
	return w.take();
}

Bytes enc(const IndexUpdate &m)
{
	Writer w;
	enc_index_common(w, m);
	w.varint_nz(4, iv(m.prev_sequence));
	return w.take();
}

Bytes enc(const Request &m)
{
	Writer w;
	w.varint_nz(1, iv(m.id));
	w.string_nz(2, m.folder);
	w.string_nz(3, m.name);
	w.varint_nz(4, iv(m.offset));
	w.varint_nz(5, iv(m.size));
	put_bytes_nz(w, 6, m.hash);
	put_bool(w, 7, m.from_temporary);
	w.varint_nz(9, iv(m.block_no));
	return w.take();
}

Bytes enc(const Response &m)
{
	Writer w;
	w.varint_nz(1, iv(m.id));
	put_bytes_nz(w, 2, m.data);
	w.varint_nz(3, ev(m.code));
	return w.take();
}

Bytes enc(const DownloadProgress &m)
{
	Writer w;
	w.string_nz(1, m.folder);
	for (const auto &u : m.updates) {
		Writer uw;
		uw.varint_nz(1, ev(u.update_type));
		uw.string_nz(2, u.name);
		if (!u.version.counters.empty())
			uw.bytes(3, encode(u.version));
		if (!u.block_indexes.empty()) {
			Writer packed;
			// A packed field is the bare varints: encode them through a throwaway writer and strip the keys.
			std::vector<uint8_t> run;
			for (int32_t i : u.block_indexes) {
				uint64_t v = iv(i);
				while (v >= 0x80) {
					run.push_back(static_cast<uint8_t>(v | 0x80));
					v >>= 7;
				}
				run.push_back(static_cast<uint8_t>(v));
			}
			uw.bytes(4, run);
		}
		uw.varint_nz(5, iv(u.block_size));
		w.bytes(2, uw.data());
	}
	return w.take();
}

Bytes enc(const Ping &) { return {}; }

Bytes enc(const Close &m)
{
	Writer w;
	w.string_nz(1, m.reason);
	return w.take();
}

// --- per-message decoders ------------------------------------------------------------------------------------------

std::optional<Message> dec_cluster_config(std::span<const uint8_t> data)
{
	ClusterConfig m;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_len(f)) {
				auto folder = dec_folder(f.bytes);
				if (!folder)
					return false;
				m.folders.push_back(std::move(*folder));
			} else if (f.number == 2 && is_varint(f)) {
				m.secondary = f.varint != 0;
			}
			return true;
		}))
		return std::nullopt;
	return m;
}

template <class IndexLike> std::optional<Message> dec_index(std::span<const uint8_t> data)
{
	IndexLike m;
	if (!each(data, [&](const Field &f) {
			switch (f.number) {
			case 1:
				if (is_len(f))
					m.folder = f.str();
				break;
			case 2:
				if (is_len(f)) {
					auto fi = dec_file(f.bytes);
					if (!fi)
						return false;
					fi->local_flags = 0; // never taken from the wire
					m.files.push_back(std::move(*fi));
				}
				break;
			case 3:
				if (is_varint(f))
					m.last_sequence = static_cast<int64_t>(f.varint);
				break;
			case 4:
				if constexpr (std::is_same_v<IndexLike, IndexUpdate>) {
					if (is_varint(f))
						m.prev_sequence = static_cast<int64_t>(f.varint);
				}
				break;
			default:
				break;
			}
			return true;
		}))
		return std::nullopt;
	if (m.folder.size() > 1024 || m.last_sequence < 0)
		return std::nullopt;
	return m;
}

std::optional<Message> dec_request(std::span<const uint8_t> data)
{
	Request m;
	if (!each(data, [&](const Field &f) {
			switch (f.number) {
			case 1:
				if (is_varint(f))
					m.id = static_cast<int32_t>(f.varint);
				break;
			case 2:
				if (is_len(f))
					m.folder = f.str();
				break;
			case 3:
				if (is_len(f))
					m.name = f.str();
				break;
			case 4:
				if (is_varint(f))
					m.offset = static_cast<int64_t>(f.varint);
				break;
			case 5:
				if (is_varint(f))
					m.size = static_cast<int32_t>(f.varint);
				break;
			case 6:
				if (is_len(f))
					m.hash = to_bytes(f.bytes);
				break;
			case 7:
				if (is_varint(f))
					m.from_temporary = f.varint != 0;
				break;
			case 9:
				if (is_varint(f))
					m.block_no = static_cast<int32_t>(f.varint);
				break;
			default:
				break;
			}
			return true;
		}))
		return std::nullopt;
	if (m.id < 0 || m.offset < 0 || m.size < 0 || m.block_no < 0 ||
		(!m.hash.empty() && m.hash.size() != 32 && m.hash.size() != 48))
		return std::nullopt;
	return m;
}

std::optional<Message> dec_response(std::span<const uint8_t> data)
{
	Response m;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_varint(f))
				m.id = static_cast<int32_t>(f.varint);
			else if (f.number == 2 && is_len(f))
				m.data = to_bytes(f.bytes);
			else if (f.number == 3 && is_varint(f))
				m.code = static_cast<ErrorCode>(f.varint);
			return true;
		}))
		return std::nullopt;
	return m;
}

std::optional<Message> dec_download_progress(std::span<const uint8_t> data)
{
	DownloadProgress m;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_len(f)) {
				m.folder = f.str();
			} else if (f.number == 2 && is_len(f)) {
				FileDownloadProgressUpdate u;
				if (!each(f.bytes, [&](const Field &g) {
						switch (g.number) {
						case 1:
							if (is_varint(g))
								u.update_type = static_cast<FileDownloadProgressUpdateType>(g.varint);
							break;
						case 2:
							if (is_len(g))
								u.name = g.str();
							break;
						case 3:
							if (is_len(g)) {
								auto v = decode_vector(g.bytes);
								if (!v)
									return false;
								u.version = std::move(*v);
							}
							break;
						case 4:
							return read_int32s(g, u.block_indexes);
						case 5:
							if (is_varint(g))
								u.block_size = static_cast<int32_t>(g.varint);
							break;
						default:
							break;
						}
						return true;
					}))
					return false;
				m.updates.push_back(std::move(u));
			}
			return true;
		}))
		return std::nullopt;
	return m;
}

std::optional<Message> dec_close(std::span<const uint8_t> data)
{
	Close m;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_len(f))
				m.reason = f.str();
			return true;
		}))
		return std::nullopt;
	return m;
}

} // namespace

// --- public --------------------------------------------------------------------------------------------------------

MessageType type_of(const Message &message)
{
	return static_cast<MessageType>(message.index());
}

Bytes encode(const Header &h)
{
	Writer w;
	w.varint_nz(1, ev(h.type));
	w.varint_nz(2, ev(h.compression));
	return w.take();
}

Bytes encode(const Message &message)
{
	return std::visit([](const auto &m) { return enc(m); }, message);
}

Bytes encode(const FileInfo &f) { return enc_file(f, true); }

Bytes encode(const Vector &v)
{
	Writer w;
	for (const auto &c : v.counters)
		w.bytes(1, enc_counter(c));
	return w.take();
}

std::optional<Header> decode_header(std::span<const uint8_t> data)
{
	Header h;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_varint(f))
				h.type = static_cast<MessageType>(f.varint);
			else if (f.number == 2 && is_varint(f))
				h.compression = static_cast<MessageCompression>(f.varint);
			return true;
		}))
		return std::nullopt;
	return h;
}

std::optional<Message> decode(MessageType type, std::span<const uint8_t> data)
{
	switch (type) {
	case MessageType::cluster_config:
		return dec_cluster_config(data);
	case MessageType::index:
		return dec_index<Index>(data);
	case MessageType::index_update:
		return dec_index<IndexUpdate>(data);
	case MessageType::request:
		return dec_request(data);
	case MessageType::response:
		return dec_response(data);
	case MessageType::download_progress:
		return dec_download_progress(data);
	case MessageType::ping:
		return Ping{};
	case MessageType::close:
		return dec_close(data);
	}
	return std::nullopt;
}

std::optional<FileInfo> decode_file_info(std::span<const uint8_t> data) { return dec_file(data); }

std::optional<Vector> decode_vector(std::span<const uint8_t> data)
{
	Vector v;
	if (!each(data, [&](const Field &f) {
			if (f.number == 1 && is_len(f)) {
				auto c = dec_counter(f.bytes);
				if (!c)
					return false;
				v.counters.push_back(*c);
			}
			return true;
		}))
		return std::nullopt;
	return v;
}

} // namespace xst::bep
