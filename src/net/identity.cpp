#include "net/identity.hpp"

#include <cstring>
#include <ctime>
#include <memory>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/random.h>

namespace xst {

namespace {

void set_error(std::string *error, const std::string &what, int code)
{
	if (error)
		*error = what + " failed: " + std::to_string(code) + " (" + wc_GetErrorString(code) + ")";
}

// GeneralNames ::= SEQUENCE { dNSName [2] IA5String }, the subjectAltName's value.
std::vector<uint8_t> dns_alt_name(const std::string &name)
{
	std::vector<uint8_t> out = { 0x30, static_cast<uint8_t>(name.size() + 2), 0x82, static_cast<uint8_t>(name.size()) };
	out.insert(out.end(), name.begin(), name.end());
	return out;
}

std::string to_pem(const std::vector<uint8_t> &der, int type)
{
	const int size = wc_DerToPem(der.data(), static_cast<word32>(der.size()), nullptr, 0, type);
	if (size <= 0)
		return "";
	std::string pem(static_cast<size_t>(size), '\0');
	const int written = wc_DerToPem(der.data(), static_cast<word32>(der.size()),
		reinterpret_cast<byte *>(pem.data()), static_cast<word32>(pem.size()), type);
	if (written <= 0)
		return "";
	pem.resize(static_cast<size_t>(written));
	return pem;
}

std::optional<std::vector<uint8_t>> from_pem_text(const std::string &pem, int type, std::string *error)
{
	DerBuffer *der = nullptr;
	const int ret = wc_PemToDer(reinterpret_cast<const unsigned char *>(pem.data()), static_cast<long>(pem.size()),
		type, &der, nullptr, nullptr, nullptr);
	if (ret != 0 || der == nullptr) {
		set_error(error, "wc_PemToDer", ret);
		return std::nullopt;
	}
	std::vector<uint8_t> out(der->buffer, der->buffer + der->length);
	wc_FreeDer(&der);
	return out;
}

} // namespace

std::optional<Identity> Identity::generate(std::string *error, const std::string &common_name, int days_valid)
{
	WC_RNG rng;
	int ret = wc_InitRng(&rng);
	if (ret != 0) {
		set_error(error, "wc_InitRng", ret);
		return std::nullopt;
	}
	struct RngGuard {
		WC_RNG *r;
		~RngGuard() { wc_FreeRng(r); }
	} rng_guard{ &rng };

	ed25519_key key;
	wc_ed25519_init(&key);
	struct KeyGuard {
		ed25519_key *k;
		~KeyGuard() { wc_ed25519_free(k); }
	} key_guard{ &key };
	ret = wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key);
	if (ret != 0) {
		set_error(error, "wc_ed25519_make_key", ret);
		return std::nullopt;
	}

	auto cert = std::make_unique<Cert>();
	wc_InitCert(cert.get());
	cert->sigType = CTC_ED25519;
	cert->daysValid = days_valid;
	cert->isCA = 0;
	cert->isCaSet = 1;
	cert->basicConstSet = 1; // emit BasicConstraints CA:FALSE, as Go's BasicConstraintsValid does
	cert->selfSigned = 1;
	std::strncpy(cert->subject.commonName, common_name.c_str(), CTC_NAME_SIZE - 1);
	std::strncpy(cert->subject.org, "Syncthing", CTC_NAME_SIZE - 1);
	std::strncpy(cert->subject.unit, "Automatically Generated", CTC_NAME_SIZE - 1);
	const auto alt = dns_alt_name(common_name);
	std::memcpy(cert->altNames, alt.data(), alt.size());
	cert->altNamesSz = static_cast<int>(alt.size());
	cert->keyUsage = KEYUSE_DIGITAL_SIG | KEYUSE_KEY_ENCIPHER;
	cert->extKeyUsage = EXTKEYUSE_SERVER_AUTH | EXTKEYUSE_CLIENT_AUTH;
	// A random positive 63-bit serial, as Syncthing's rand.Uint64 (positive in DER needs the top bit clear).
	byte serial[8];
	wc_RNG_GenerateBlock(&rng, serial, sizeof serial);
	serial[0] &= 0x7F;
	if (serial[0] == 0)
		serial[0] = 1;
	std::memcpy(cert->serial, serial, sizeof serial);
	cert->serialSz = sizeof serial;

	std::vector<uint8_t> der(4096);
	ret = wc_MakeCert_ex(cert.get(), der.data(), static_cast<word32>(der.size()), ED25519_TYPE, &key, &rng);
	if (ret < 0) {
		set_error(error, "wc_MakeCert_ex", ret);
		return std::nullopt;
	}
	ret = wc_SignCert_ex(cert->bodySz, cert->sigType, der.data(), static_cast<word32>(der.size()), ED25519_TYPE, &key,
		&rng);
	if (ret < 0) {
		set_error(error, "wc_SignCert_ex", ret);
		return std::nullopt;
	}
	der.resize(static_cast<size_t>(ret));

	std::vector<uint8_t> key_der(256);
	ret = wc_Ed25519PrivateKeyToDer(&key, key_der.data(), static_cast<word32>(key_der.size()));
	if (ret < 0) {
		set_error(error, "wc_Ed25519PrivateKeyToDer", ret);
		return std::nullopt;
	}
	key_der.resize(static_cast<size_t>(ret));

	Identity identity;
	identity.cert_der = std::move(der);
	identity.key_der = std::move(key_der);
	identity.id = DeviceId::from_certificate(identity.cert_der);
	return identity;
}

std::optional<Identity> Identity::from_pem(const std::string &cert_pem, const std::string &key_pem, std::string *error)
{
	auto cert = from_pem_text(cert_pem, CERT_TYPE, error);
	if (!cert)
		return std::nullopt;
	// Keep the PKCS#8 wrapper (PRIVATEKEY_TYPE would strip it to the bare key, which the TLS loader can't type).
	// Syncthing writes Ed25519 keys as PKCS#8 "PRIVATE KEY", and older ECDSA ones as "EC PRIVATE KEY".
	auto key = key_pem.find("BEGIN PRIVATE KEY") != std::string::npos
		? from_pem_text(key_pem, PKCS8_PRIVATEKEY_TYPE, error)
		: from_pem_text(key_pem, ECC_PRIVATEKEY_TYPE, error);
	if (!key)
		return std::nullopt;
	Identity identity;
	identity.cert_der = std::move(*cert);
	identity.key_der = std::move(*key);
	identity.id = DeviceId::from_certificate(identity.cert_der);
	return identity;
}

std::string Identity::cert_pem() const { return to_pem(cert_der, CERT_TYPE); }
std::string Identity::key_pem() const
{
	// PKCS#8 starts SEQUENCE { INTEGER version, SEQUENCE algorithm, ... }; SEC1 EC keys start SEQUENCE { INTEGER 1,
	// OCTET STRING ... }.
	const bool sec1 = key_der.size() > 5 && key_der[2] == 0x02 && key_der[3] == 0x01 && key_der[4] == 0x01 &&
		key_der[5] == 0x04;
	return to_pem(key_der, sec1 ? ECC_PRIVATEKEY_TYPE : PKCS8_PRIVATEKEY_TYPE);
}

} // namespace xst
