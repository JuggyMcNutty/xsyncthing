#pragma once

#include <string_view>

namespace xst {

// The name and version sent to peers in the BEP Hello message.
std::string_view client_name();
std::string_view client_version();

} // namespace xst
