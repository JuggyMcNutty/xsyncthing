#pragma once

#include <cstdint>

#include "core/bep.hpp"

// Version vectors, as lib/protocol/vector.go: counters sorted by ID; a device bumps its own counter to
// max(old + 1, now in Unix seconds) when it changes a file.
namespace xst::vec {

enum class Ordering { equal, greater, lesser, concurrent_lesser, concurrent_greater };

Ordering compare(const bep::Vector &a, const bep::Vector &b);
inline bool concurrent(const bep::Vector &a, const bep::Vector &b)
{
	const auto o = compare(a, b);
	return o == Ordering::concurrent_lesser || o == Ordering::concurrent_greater;
}
inline bool greater_equal(const bep::Vector &a, const bep::Vector &b)
{
	const auto o = compare(a, b);
	return o == Ordering::greater || o == Ordering::equal;
}
inline bool equal(const bep::Vector &a, const bep::Vector &b) { return compare(a, b) == Ordering::equal; }

bep::Vector update(bep::Vector v, uint64_t id, uint64_t now_unix);
bep::Vector update(bep::Vector v, uint64_t id); // now = the wall clock
bep::Vector merge(bep::Vector a, const bep::Vector &b);
uint64_t counter(const bep::Vector &v, uint64_t id);

} // namespace xst::vec
