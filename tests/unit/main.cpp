#include <cstdio>
#include <stdexcept>

#include "test.hpp"

namespace xst::test {

std::vector<Case> &registry()
{
	static std::vector<Case> cases;
	return cases;
}

struct Failure : std::runtime_error {
	using std::runtime_error::runtime_error;
};

void fail(const char *file, int line, const std::string &what)
{
	throw Failure(std::string(file) + ":" + std::to_string(line) + ": CHECK failed: " + what);
}

} // namespace xst::test

int main()
{
	int failed = 0;
	for (const auto &c : xst::test::registry()) {
		try {
			c.fn();
			std::printf("pass %s\n", c.name);
		} catch (const std::exception &e) {
			std::printf("FAIL %s: %s\n", c.name, e.what());
			failed++;
		}
	}
	std::printf("%zu tests, %d failed\n", xst::test::registry().size(), failed);
	return failed ? 1 : 0;
}
