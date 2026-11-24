#include "core/events.hpp"

#include <algorithm>
#include <ctime>

namespace xst {

std::string rfc3339(int64_t unix_s, int32_t ns)
{
	const std::time_t t = static_cast<std::time_t>(unix_s);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[64];
	std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
	char frac[16];
	std::snprintf(frac, sizeof frac, ".%09dZ", ns);
	return std::string(buf) + frac;
}

std::string rfc3339_now()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto s = std::chrono::duration_cast<std::chrono::seconds>(now);
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - s);
	return rfc3339(s.count(), static_cast<int32_t>(ns.count()));
}

void Events::log(const std::string &type, nlohmann::json data)
{
	{
		std::lock_guard lock(mu_);
		Event e;
		e.id = next_id_++;
		e.global_id = e.id;
		e.time = rfc3339_now();
		e.type = type;
		e.data = std::move(data);
		buffer_.push_back(std::move(e));
		while (buffer_.size() > 1000)
			buffer_.pop_front();
	}
	cv_.notify_all();
}

void Events::close()
{
	{
		std::lock_guard lock(mu_);
		closed_ = true;
	}
	cv_.notify_all();
}

int64_t Events::last_id()
{
	std::lock_guard lock(mu_);
	return next_id_ - 1;
}

std::vector<Events::Event> Events::since(int64_t since, const std::vector<std::string> &types,
	std::chrono::milliseconds timeout, size_t limit)
{
	std::unique_lock lock(mu_);
	auto collect = [&] {
		std::vector<Event> out;
		for (const auto &e : buffer_) {
			if (e.id <= since)
				continue;
			if (!types.empty() && std::find(types.begin(), types.end(), e.type) == types.end())
				continue;
			out.push_back(e);
		}
		if (limit && out.size() > limit)
			out.erase(out.begin(), out.end() - static_cast<long>(limit));
		return out;
	};
	auto out = collect();
	if (!out.empty() || closed_)
		return out;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (out.empty() && !closed_ && cv_.wait_until(lock, deadline) != std::cv_status::timeout)
		out = collect();
	if (out.empty())
		out = collect();
	return out;
}

nlohmann::json Events::to_json(const Event &e)
{
	return { { "id", e.id }, { "globalID", e.global_id }, { "time", e.time }, { "type", e.type }, { "data", e.data } };
}

} // namespace xst
