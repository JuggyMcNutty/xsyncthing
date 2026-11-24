#include "core/ignore.hpp"
#include "core/vector.hpp"
#include "test.hpp"

using namespace xst;
using bep::Vector;
using vec::Ordering;

TEST(vector_compare_cases)
{
	// From upstream lib/protocol/vector_test.go's ordering table.
	const Vector empty;
	const Vector a1{ { { 1, 1 } } };
	const Vector a2{ { { 1, 2 } } };
	const Vector b1{ { { 2, 1 } } };
	const Vector a1b1{ { { 1, 1 }, { 2, 1 } } };
	const Vector a2b1{ { { 1, 2 }, { 2, 1 } } };
	const Vector a1b2{ { { 1, 1 }, { 2, 2 } } };
	const Vector zero{ { { 1, 0 } } };
	CHECK(vec::compare(empty, empty) == Ordering::equal);
	CHECK(vec::compare(empty, zero) == Ordering::equal);
	CHECK(vec::compare(a1, empty) == Ordering::greater);
	CHECK(vec::compare(empty, a1) == Ordering::lesser);
	CHECK(vec::compare(a2, a1) == Ordering::greater);
	CHECK(vec::compare(a1, a2) == Ordering::lesser);
	CHECK(vec::concurrent(a1, b1));
	CHECK(vec::compare(a1b1, a1) == Ordering::greater);
	CHECK(vec::concurrent(a2b1, a1b2));
	// As upstream's Compare returns them (checked against lib/protocol with go run).
	CHECK(vec::compare(a2b1, a1b2) == Ordering::concurrent_greater);
	CHECK(vec::compare(a1b2, a2b1) == Ordering::concurrent_lesser);
}

TEST(vector_update_and_merge)
{
	Vector v;
	v = vec::update(v, 5, 100);
	CHECK_EQ(vec::counter(v, 5), 100u);
	v = vec::update(v, 5, 50); // the clock went back: still +1
	CHECK_EQ(vec::counter(v, 5), 101u);
	v = vec::update(v, 2, 7);
	CHECK_EQ(v.counters[0].id, 2u); // kept sorted
	const Vector m = vec::merge(Vector{ { { 1, 9 }, { 5, 3 } } }, v);
	CHECK_EQ(m.counters.size(), 3u);
	CHECK_EQ(vec::counter(m, 5), 101u);
	CHECK_EQ(vec::counter(m, 1), 9u);
}

TEST(glob_basics)
{
	Glob g;
	CHECK(g.compile("*.txt") && g.match("a.txt") && !g.match("d/a.txt"));
	CHECK(g.compile("**.txt") && g.match("d/e/a.txt"));
	CHECK(g.compile("a?c") && g.match("abc") && !g.match("a/c"));
	CHECK(g.compile("[a-c]x") && g.match("bx") && !g.match("dx"));
	CHECK(g.compile("[!a-c]x") && g.match("dx") && !g.match("ax"));
	CHECK(g.compile("{foo,ba{r,z}}.c") && g.match("foo.c") && g.match("baz.c") && !g.match("bay.c"));
	CHECK(g.compile("\\*x") && g.match("*x") && !g.match("ax"));
	CHECK(!g.compile("[abc"));
	CHECK(!g.compile("{a,b"));
}

TEST(ignore_semantics)
{
	IgnoreMatcher m;
	std::string error;
	CHECK(m.parse("// comment\n*.tmp\n/rootonly\n!keep.tmp\n(?i)CASE\nbuild/\n(?d).DS_Store\n", nullptr, &error));
	// First match wins, so the include must come before the general rule to matter: here it's after.
	CHECK(m.match("x.tmp").ignored);
	CHECK(m.match("dir/x.tmp").ignored);
	CHECK(m.match("rootonly").ignored);
	CHECK(!m.match("dir/rootonly").ignored);
	CHECK(m.match("case").ignored && m.match("a/CaSe").ignored);
	CHECK(m.match("build/out.o").ignored);
	CHECK(!m.match("build").ignored); // "build/" matches the contents only
	CHECK(m.match(".DS_Store").deletable);
	CHECK(m.match("dir/sub").ignored == false);
	// A plain pattern also covers everything below what it matches.
	IgnoreMatcher n;
	CHECK(n.parse("!important.tmp\n*.tmp\nnode_modules\n", nullptr, &error));
	CHECK(!n.match("important.tmp").ignored);
	CHECK(n.match("a.tmp").ignored);
	CHECK(n.match("x/node_modules/y/z.js").ignored);
	CHECK(!n.match("a.tmp").can_skip_dir); // an include exists
}

TEST(ignore_internal_and_temporary)
{
	IgnoreMatcher m;
	CHECK(m.match(".stfolder").ignored);
	CHECK(m.match(".stversions/a").ignored);
	CHECK(m.match("d/.syncthing.file.tmp").ignored);
	CHECK(m.match("d/.st~0a1b2c3d.tmp").ignored);
	CHECK(!m.match("stfolder").ignored);
}

TEST(ignore_include_file)
{
	IgnoreMatcher m;
	std::string error;
	const auto reader = [](const std::string &path) -> std::optional<std::string> {
		if (path == "more.txt")
			return std::string("*.bak\n");
		return std::nullopt;
	};
	CHECK(m.parse("#include more.txt\n", reader, &error));
	CHECK(m.match("x.bak").ignored);
	CHECK(!m.parse("#include missing.txt\n", reader, &error));
}
