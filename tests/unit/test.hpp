#pragma once

#include <functional>
#include <string>
#include <vector>

namespace xst::test {

struct Case {
	const char *name;
	std::function<void()> fn;
};

std::vector<Case> &registry();
void fail(const char *file, int line, const std::string &what);

struct Register {
	Register(const char *name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};

} // namespace xst::test

#define XST_CAT2(a, b) a##b
#define XST_CAT(a, b) XST_CAT2(a, b)
#define TEST(name)                                                                                                 \
	static void name();                                                                                            \
	static ::xst::test::Register XST_CAT(reg_, name)(#name, name);                                                 \
	static void name()
#define CHECK(cond)                                                                                                \
	do {                                                                                                           \
		if (!(cond))                                                                                               \
			::xst::test::fail(__FILE__, __LINE__, #cond);                                                          \
	} while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
