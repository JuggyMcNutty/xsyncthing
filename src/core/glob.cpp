#include "core/glob.hpp"

namespace xst {

namespace {

using Node = Glob::Node;
using Seq = std::vector<Node>;

struct Parser {
	std::string_view p;
	size_t i = 0;
	std::string error;

	// Parses until end, or (inside braces) an unescaped ',' or '}' at this level.
	bool seq(Seq &out, bool in_braces)
	{
		while (i < p.size()) {
			const char c = p[i];
			if (in_braces && (c == ',' || c == '}'))
				return true;
			if (c == '*') {
				if (i + 1 < p.size() && p[i + 1] == '*') {
					out.push_back(Node{ .kind = Node::super_star });
					i += 2;
				} else {
					out.push_back(Node{ .kind = Node::star });
					i++;
				}
			} else if (c == '?') {
				out.push_back(Node{ .kind = Node::any });
				i++;
			} else if (c == '[') {
				if (!cls(out))
					return false;
			} else if (c == '{') {
				if (!alt(out))
					return false;
			} else if (c == '\\') {
				if (i + 1 >= p.size()) {
					error = "trailing backslash";
					return false;
				}
				lit(out, p[i + 1]);
				i += 2;
			} else {
				lit(out, c);
				i++;
			}
		}
		if (in_braces) {
			error = "unclosed {";
			return false;
		}
		return true;
	}

	static void lit(Seq &out, char c)
	{
		if (!out.empty() && out.back().kind == Node::literal)
			out.back().text.push_back(c);
		else
			out.push_back(Node{ .kind = Node::literal, .text = std::string(1, c) });
	}

	bool cls(Seq &out)
	{
		Node n{ .kind = Node::cls };
		i++; // [
		if (i < p.size() && p[i] == '!') {
			n.negated = true;
			i++;
		}
		bool first = true;
		while (i < p.size() && (p[i] != ']' || first)) {
			first = false;
			unsigned char lo = static_cast<unsigned char>(p[i]);
			if (p[i] == '\\' && i + 1 < p.size())
				lo = static_cast<unsigned char>(p[++i]);
			i++;
			unsigned char hi = lo;
			if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
				hi = static_cast<unsigned char>(p[i + 1]);
				if (p[i + 1] == '\\' && i + 2 < p.size()) {
					hi = static_cast<unsigned char>(p[i + 2]);
					i++;
				}
				i += 2;
			}
			n.ranges.emplace_back(lo, hi);
		}
		if (i >= p.size()) {
			error = "unclosed [";
			return false;
		}
		i++; // ]
		out.push_back(std::move(n));
		return true;
	}

	bool alt(Seq &out)
	{
		Node n{ .kind = Node::alt };
		i++; // {
		for (;;) {
			Seq s;
			if (!seq(s, true))
				return false;
			n.alternatives.push_back(std::move(s));
			if (p[i] == '}') {
				i++;
				break;
			}
			i++; // ,
		}
		out.push_back(std::move(n));
		return true;
	}
};

bool in_class(const Node &n, unsigned char c)
{
	bool hit = false;
	for (const auto &[lo, hi] : n.ranges)
		if (c >= lo && c <= hi) {
			hit = true;
			break;
		}
	return hit != n.negated;
}

// Matches nodes[ni..] followed by rest (a continuation stack for alternatives) against text[ti..].
struct Cont {
	const Seq *seq;
	size_t index;
	const Cont *next;
};

bool match_at(const Seq &seq, size_t ni, std::string_view t, size_t ti, const Cont *rest);

bool match_cont(const Cont *rest, std::string_view t, size_t ti)
{
	if (!rest)
		return ti == t.size();
	return match_at(*rest->seq, rest->index, t, ti, rest->next);
}

bool match_at(const Seq &seq, size_t ni, std::string_view t, size_t ti, const Cont *rest)
{
	while (ni < seq.size()) {
		const Node &n = seq[ni];
		switch (n.kind) {
		case Node::literal:
			if (t.substr(ti, n.text.size()) != n.text)
				return false;
			ti += n.text.size();
			ni++;
			break;
		case Node::any:
			if (ti >= t.size() || t[ti] == '/')
				return false;
			ti++;
			ni++;
			break;
		case Node::cls:
			if (ti >= t.size() || t[ti] == '/' || !in_class(n, static_cast<unsigned char>(t[ti])))
				return false;
			ti++;
			ni++;
			break;
		case Node::star:
			for (size_t k = ti;; k++) {
				if (match_at(seq, ni + 1, t, k, rest))
					return true;
				if (k >= t.size() || t[k] == '/')
					return false;
			}
		case Node::super_star:
			for (size_t k = ti; k <= t.size(); k++)
				if (match_at(seq, ni + 1, t, k, rest))
					return true;
			return false;
		case Node::alt: {
			const Cont after{ &seq, ni + 1, rest };
			for (const auto &a : n.alternatives)
				if (match_at(a, 0, t, ti, &after))
					return true;
			return false;
		}
		}
	}
	return match_cont(rest, t, ti);
}

} // namespace

bool Glob::compile(std::string_view pattern, std::string *error)
{
	Parser parser;
	parser.p = pattern;
	Seq seq;
	if (!parser.seq(seq, false)) {
		if (error)
			*error = parser.error;
		return false;
	}
	nodes_ = std::move(seq);
	return true;
}

bool Glob::match(std::string_view text) const { return match_at(nodes_, 0, text, 0, nullptr); }

} // namespace xst
