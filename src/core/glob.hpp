#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace xst {

// Glob matching as Syncthing's ignore patterns use it (github.com/gobwas/glob with '/' as the separator):
//   *  any run of characters other than '/'      ** any run of characters, '/' included
//   ?  one character other than '/'              [abc] [a-z] [!abc] [!a-z]  one character of (not of) a class
//   {a,b,c}  one of the alternatives (which may hold the others)    \x  x itself
// Matches whole strings. Works on bytes, so non-ASCII text matches literally (classes are byte-wise).
class Glob {
public:
	// false (and error) if the pattern is malformed: an unclosed [ or {, or a trailing \.
	bool compile(std::string_view pattern, std::string *error = nullptr);
	bool match(std::string_view text) const;

	// The compiled form; public so the matcher's helpers can walk it.
	struct Node {
		enum Kind { literal, star, super_star, any, cls, alt };
		Kind kind = literal;
		std::string text = {};                                             // literal
		std::vector<std::pair<unsigned char, unsigned char>> ranges = {}; // cls
		bool negated = false;                                              // cls
		std::vector<std::vector<Node>> alternatives = {};                  // alt: each a sequence
	};

private:
	std::vector<Node> nodes_;
};

} // namespace xst
