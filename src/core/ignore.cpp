#include "core/ignore.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace xst {

namespace {

std::string lower(std::string_view s)
{
	std::string out(s);
	for (auto &c : out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

std::string base_name(std::string_view path)
{
	const auto slash = path.rfind('/');
	return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

} // namespace

bool IgnoreMatcher::is_internal(std::string_view path)
{
	for (std::string_view internal : { ".stfolder", ".stignore", ".stversions" })
		if (path == internal || (path.size() > internal.size() && path.starts_with(internal) &&
									path[internal.size()] == '/'))
			return true;
	return false;
}

bool IgnoreMatcher::is_temporary(std::string_view path)
{
	const std::string base = base_name(path);
	// Upstream's two forms, and xsyncthing's short one for 42-character FATX names.
	return base.starts_with(".syncthing.") || base.starts_with("~syncthing~") || base.starts_with(".st~");
}

bool IgnoreMatcher::add_line(std::string line, std::string *error)
{
	Pattern base;
	bool seen[3] = {};
	for (;;) {
		if (line.starts_with("!") && !seen[0]) {
			seen[0] = true;
			base.include = true;
			line.erase(0, 1);
		} else if (line.starts_with("(?i)") && !seen[1]) {
			seen[1] = true;
			base.fold_case = true;
			line.erase(0, 4);
		} else if (line.starts_with("(?d)") && !seen[2]) {
			seen[2] = true;
			base.deletable = true;
			line.erase(0, 4);
		} else {
			break;
		}
	}
	if (line.empty()) {
		if (error)
			*error = "missing pattern";
		return false;
	}
	if (base.fold_case)
		line = lower(line);
	if (base.include)
		has_includes_ = true;

	auto add = [&](std::string_view glob) {
		Pattern p = base;
		std::string e;
		if (!p.glob.compile(glob, &e)) {
			if (error)
				*error = "invalid pattern \"" + line + "\": " + e;
			return false;
		}
		patterns_.push_back(std::move(p));
		return true;
	};
	if (line.starts_with("/"))
		return add(std::string_view(line).substr(1));
	if (line.starts_with("**/")) {
		if (!add(line))
			return false;
		const std::string rest = line.substr(3);
		return rest.empty() || add(rest);
	}
	return add(line) && add("**/" + line);
}

bool IgnoreMatcher::parse_file(std::string_view text, const std::string &file,
	const std::function<std::optional<std::string>(const std::string &)> &read_file, int depth, std::string *error)
{
	if (depth > 8) {
		if (error)
			*error = "#include nested too deeply";
		return false;
	}
	std::set<std::string> seen;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string_view::npos)
			end = text.size();
		std::string line(text.substr(pos, end - pos));
		pos = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		if (line.empty() || line.starts_with("//") || !seen.insert(line).second) {
			if (pos > text.size())
				break;
			continue;
		}
		if (depth == 0)
			lines_.push_back(line);
		bool ok;
		if (line.starts_with("#include")) {
			const auto space = line.find(' ');
			std::string inc = space == std::string::npos ? "" : line.substr(space + 1);
			while (!inc.empty() && inc.front() == ' ')
				inc.erase(0, 1);
			if (inc.empty()) {
				if (error)
					*error = "failed to parse #include line: no file?";
				return false;
			}
			const auto dir_end = file.rfind('/');
			const std::string path = dir_end == std::string::npos ? inc : file.substr(0, dir_end + 1) + inc;
			const auto content = read_file ? read_file(path) : std::nullopt;
			if (!content) {
				if (error)
					*error = "failed to load include file " + path;
				return false;
			}
			ok = parse_file(*content, path, read_file, depth + 1, error);
		} else if (line.ends_with("/**")) {
			ok = add_line(line, error);
		} else if (line.ends_with("/")) {
			ok = add_line(line + "**", error);
		} else {
			ok = add_line(line, error) && add_line(line + "/**", error);
		}
		if (!ok)
			return false;
		if (pos > text.size())
			break;
	}
	return true;
}

bool IgnoreMatcher::parse(std::string_view text,
	const std::function<std::optional<std::string>(const std::string &)> &read_file, std::string *error)
{
	patterns_.clear();
	lines_.clear();
	has_includes_ = false;
	if (!parse_file(text, ".stignore", read_file, 0, error)) {
		patterns_.clear();
		return false;
	}
	return true;
}

IgnoreMatcher::Result IgnoreMatcher::match(std::string_view path) const
{
	if (is_temporary(path) || is_internal(path))
		return { true, false, true };
	std::string folded;
	for (const auto &p : patterns_) {
		bool hit;
		if (p.fold_case) {
			if (folded.empty())
				folded = lower(path);
			hit = p.glob.match(folded);
		} else {
			hit = p.glob.match(path);
		}
		if (hit)
			return { !p.include, p.deletable, !has_includes_ };
	}
	return {};
}

} // namespace xst
