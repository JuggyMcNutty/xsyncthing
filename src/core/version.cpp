#include "core/version.hpp"

#include "xst/version.h"

namespace xst {

std::string_view client_name() { return kClientName; }
std::string_view client_version() { return kVersion; }

} // namespace xst
