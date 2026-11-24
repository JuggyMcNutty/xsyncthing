#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/glob.hpp"

namespace xst {

// .stignore, as lib/ignore: one pattern a line, first match wins.
//   !pattern  include (don't ignore)     (?i)  case-insensitive      (?d)  may be deleted to remove a directory
//   /pattern  only at the folder root    dir/  the directory's contents     // comment     #include file
// A pattern without a leading / matches at any depth; a pattern also matches everything under what it matches.
class IgnoreMatcher {
public:
	struct Result {
		bool ignored = false;
		bool deletable = false;
		bool can_skip_dir = false; // nothing inside an ignored directory can be un-ignored
	};

	// Reads patterns from text; includes are read with read_file(path relative to the folder). false on a bad
	// pattern, with error set; the matcher then ignores nothing (upstream refuses to scan in that case).
	bool parse(std::string_view text, const std::function<std::optional<std::string>(const std::string &)> &read_file,
		std::string *error = nullptr);

	Result match(std::string_view path) const;
	bool empty() const { return patterns_.empty(); }
	const std::vector<std::string> &lines() const { return lines_; }

	// Names Syncthing never syncs: its own files and temporaries.
	static bool is_internal(std::string_view path);
	static bool is_temporary(std::string_view path);

private:
	struct Pattern {
		Glob glob;
		bool include = false;
		bool fold_case = false;
		bool deletable = false;
	};
	bool add_line(std::string line, std::string *error);
	bool parse_file(std::string_view text, const std::string &file,
		const std::function<std::optional<std::string>(const std::string &)> &read_file, int depth,
		std::string *error);

	std::vector<Pattern> patterns_;
	std::vector<std::string> lines_;
	bool has_includes_ = false;
};

} // namespace xst
