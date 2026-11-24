#pragma once

#include <string>

namespace xst::console {

// Moves the calling thread to a hardware thread (0..5). The UI keeps 0; networking and the model share 2-3;
// hashing runs on 4-5 (core/scanner's workers ask through this).
void place_current_thread(unsigned hardware_thread);

// Runs fn when the user leaves the title through Xbox Home: the title is frozen then, so fn may only flag work for a
// thread of its own and must not take locks the title's threads may hold.
void on_home(void (*fn)());

// The console's IPv4 address as text, or a state ("getting an address...", "no network").
std::string address();

} // namespace xst::console
