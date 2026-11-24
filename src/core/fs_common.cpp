#include "core/fs.hpp"

namespace xst::fs {

std::string_view parent(std::string_view path)
{
	const auto slash = path.rfind('/');
	return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

std::string_view base(std::string_view path)
{
	const auto slash = path.rfind('/');
	return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string join(std::string_view a, std::string_view b)
{
	if (a.empty())
		return std::string(b);
	if (b.empty())
		return std::string(a);
	std::string out(a);
	if (out.back() != '/')
		out.push_back('/');
	out.append(b);
	return out;
}

std::string unstorable_reason(std::string_view path, int64_t size, const VolumeRules &rules)
{
	size_t start = 0;
	while (start <= path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string_view::npos)
			end = path.size();
		const auto part = path.substr(start, end - start);
		if (part.size() > rules.max_name_bytes)
			return "name too long for " + rules.type + " (" + std::to_string(part.size()) + " > " +
				std::to_string(rules.max_name_bytes) + " bytes): " + std::string(part);
		for (char c : part) {
			if (static_cast<unsigned char>(c) < 0x20 && rules.type != "posix")
				return "name contains a control character, which " + rules.type + " can't store";
			if (rules.forbidden_chars.find(c) != std::string::npos)
				return std::string("name contains '") + c + "', which " + rules.type + " can't store";
		}
		if (rules.type != "posix" && !part.empty() && (part.back() == '.' || part.back() == ' '))
			return "name ends with a dot or space, which " + rules.type + " can't store";
		if (end == path.size())
			break;
		start = end + 1;
	}
	if (size > rules.max_file_size)
		return "file too large for " + rules.type + " (" + std::to_string(size) + " bytes)";
	return "";
}

} // namespace xst::fs
