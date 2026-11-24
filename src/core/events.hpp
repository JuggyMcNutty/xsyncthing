#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace xst {

// Syncthing's event log (lib/events): numbered events of a type with JSON data, which /rest/events long-polls.
// Keeps the last 1000.
class Events {
public:
	struct Event {
		int64_t id = 0;
		int64_t global_id = 0;
		std::string time; // RFC 3339
		std::string type;
		nlohmann::json data;
	};

	void log(const std::string &type, nlohmann::json data);
	// Events with id > since and type in types (all if empty), waiting up to timeout for at least one; at most limit.
	std::vector<Event> since(int64_t since, const std::vector<std::string> &types, std::chrono::milliseconds timeout,
		size_t limit = 0);
	int64_t last_id();
	// Wakes every waiting since() now and makes later ones return at once (on shutdown).
	void close();

	static nlohmann::json to_json(const Event &e);

private:
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<Event> buffer_;
	int64_t next_id_ = 1;
	bool closed_ = false;
};

std::string rfc3339_now();
std::string rfc3339(int64_t unix_s, int32_t ns = 0);

} // namespace xst
