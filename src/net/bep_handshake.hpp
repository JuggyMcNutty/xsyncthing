#pragma once

#include <memory>
#include <string>

#include "core/hello.hpp"
#include "net/tls.hpp"

namespace xst {

inline constexpr const char *kBepAlpn = "bep/1.0";

// TLS options for a BEP connection: TLS 1.3, ALPN bep/1.0, our certificate, the peer's required.
TlsOptions bep_tls_options(const Identity &identity, TlsOptions::Role role);

// Sends ours and reads the peer's Hello over an established BEP TLS connection.
std::optional<Hello> exchange_hello(TlsConnection &conn, const Hello &ours, std::string *error = nullptr);

} // namespace xst
