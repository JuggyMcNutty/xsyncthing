#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/device_id.hpp"

namespace xst {

// This device's TLS identity: a self-signed certificate and its private key, as Syncthing makes them
// (lib/tlsutil generateCertificate): Ed25519, CN and DNS name "syncthing", O "Syncthing", OU "Automatically
// Generated", key usage digital signature + key encipherment, extended key usage server + client auth, 20 years.
struct Identity {
	std::vector<uint8_t> cert_der;
	std::vector<uint8_t> key_der; // PKCS#8
	DeviceId id;

	// A new identity; nullopt if the RNG or certificate generation failed (error says why).
	static std::optional<Identity> generate(std::string *error = nullptr, const std::string &common_name = "syncthing",
		int days_valid = 20 * 365);
	// An ECDSA P-256 identity, for the GUI's HTTPS (browsers don't take Ed25519; lib/tlsutil "compatible").
	static std::optional<Identity> generate_ecdsa_p256(std::string *error = nullptr,
		const std::string &common_name = "syncthing", int days_valid = 820);
	// Loads cert.pem and key.pem as Syncthing writes them.
	static std::optional<Identity> from_pem(const std::string &cert_pem, const std::string &key_pem,
		std::string *error = nullptr);

	std::string cert_pem() const;
	std::string key_pem() const;
};

} // namespace xst
