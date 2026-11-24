#include "core/vector.hpp"

#include <algorithm>
#include <ctime>

namespace xst::vec {

Ordering compare(const bep::Vector &a, const bep::Vector &b)
{
	const auto &av = a.counters;
	const auto &bv = b.counters;
	size_t ai = 0, bi = 0;
	Ordering result = Ordering::equal;
	while (ai < av.size() || bi < bv.size()) {
		const bool a_missing = ai >= av.size();
		const bool b_missing = bi >= bv.size();
		const bep::Counter ac = a_missing ? bep::Counter{} : av[ai];
		const bep::Counter bc = b_missing ? bep::Counter{} : bv[bi];
		if (!a_missing && !b_missing && ac.id == bc.id) {
			if (ac.value > bc.value) {
				if (result == Ordering::lesser)
					return Ordering::concurrent_lesser;
				result = Ordering::greater;
			} else if (ac.value < bc.value) {
				if (result == Ordering::greater)
					return Ordering::concurrent_greater;
				result = Ordering::lesser;
			}
		} else if ((!a_missing && ac.id < bc.id) || b_missing) {
			if (ac.value > 0) {
				if (result == Ordering::lesser)
					return Ordering::concurrent_lesser;
				result = Ordering::greater;
			}
		} else if ((!b_missing && bc.id < ac.id) || a_missing) {
			if (bc.value > 0) {
				if (result == Ordering::greater)
					return Ordering::concurrent_greater;
				result = Ordering::lesser;
			}
		}
		if (!a_missing && (b_missing || ac.id <= bc.id))
			ai++;
		if (!b_missing && (a_missing || bc.id <= ac.id))
			bi++;
	}
	return result;
}

bep::Vector update(bep::Vector v, uint64_t id, uint64_t now)
{
	auto &c = v.counters;
	for (size_t i = 0; i < c.size(); i++) {
		if (c[i].id == id) {
			c[i].value = std::max(c[i].value + 1, now);
			return v;
		}
		if (c[i].id > id) {
			c.insert(c.begin() + static_cast<long>(i), bep::Counter{ id, std::max<uint64_t>(1, now) });
			return v;
		}
	}
	c.push_back(bep::Counter{ id, std::max<uint64_t>(1, now) });
	return v;
}

bep::Vector update(bep::Vector v, uint64_t id)
{
	return update(std::move(v), id, static_cast<uint64_t>(std::time(nullptr)));
}

bep::Vector merge(bep::Vector a, const bep::Vector &b)
{
	for (const auto &bc : b.counters) {
		auto it = std::lower_bound(a.counters.begin(), a.counters.end(), bc.id,
			[](const bep::Counter &c, uint64_t id) { return c.id < id; });
		if (it != a.counters.end() && it->id == bc.id)
			it->value = std::max(it->value, bc.value);
		else
			a.counters.insert(it, bc);
	}
	return a;
}

uint64_t counter(const bep::Vector &v, uint64_t id)
{
	for (const auto &c : v.counters)
		if (c.id == id)
			return c.value;
	return 0;
}

} // namespace xst::vec
