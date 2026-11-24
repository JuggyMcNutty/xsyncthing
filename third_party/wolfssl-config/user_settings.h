/* wolfSSL settings for xsyncthing on the Xbox 360 (xsdk) and on the host.
 * TLS 1.3 + 1.2, Ed25519/ECDSA-P256/RSA, AES-GCM, ChaCha20-Poly1305, SHA-2, X.509 generation. */
#ifndef XST_WOLFSSL_USER_SETTINGS_H
#define XST_WOLFSSL_USER_SETTINGS_H

/* --- Platform --- */
#ifdef __XSDK__
	/* Xenon: 32-bit ABI, big-endian, newlib, xsdk's POSIX sockets and pthreads. */
	#define BIG_ENDIAN_ORDER
	#define SIZEOF_LONG 4
	#define SIZEOF_LONG_LONG 8
	#define WOLFSSL_NO_ASM
	#define NO_WRITEV
	#define NO_DEV_RANDOM
	/* The seed comes from xsdk's getentropy (the kernel's RNG); see xst_wolfssl_seed.c. */
	extern int xst_wolfssl_seed(unsigned char *output, unsigned int size);
	#define CUSTOM_RAND_GENERATE_SEED xst_wolfssl_seed
	/* xsdk has no IPv6. */
	#define WOLFSSL_NO_IPV6
#endif

#define WOLFSSL_PTHREADS
/* tls13.c calls gettimeofday but only includes <sys/time.h> on a few OSes. */
#ifndef __ASSEMBLER__
#include <sys/time.h>
#endif
#define HAVE_PTHREAD
#define WOLFSSL_IGNORE_FILE_WARN

/* --- Math --- */
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_4096
#define TFM_TIMING_RESISTANT
#define ECC_TIMING_RESISTANT
#define WC_RSA_BLINDING

/* --- TLS --- */
#define WOLFSSL_TLS13
#define WOLFSSL_NO_TLS12_BELOW /* nothing older than 1.2 */
#define NO_OLD_TLS
#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_EXTENDED_MASTER
#define HAVE_ENCRYPT_THEN_MAC
#define HAVE_SNI
#define HAVE_ALPN
#define HAVE_SESSION_TICKET
#define HAVE_HKDF
#define HAVE_FFDHE_2048
#define WC_RSA_PSS
#define WOLFSSL_ALWAYS_VERIFY_CB
#define WOLFSSL_VERIFY_CB_ALL_CERTS
/* Syncthing peers present self-signed certs: verification is by device ID (SHA-256 of the DER) in our callback. */
#define KEEP_PEER_CERT
#define SESSION_CERTS

/* --- Algorithms --- */
#define HAVE_ECC
#define ECC_SHAMIR
#define HAVE_ED25519
#define HAVE_CURVE25519
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define WOLFSSL_SHA224
#define HAVE_AESGCM
#define WOLFSSL_AES_DIRECT
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_AEAD
#define WOLFSSL_BASE64_ENCODE

/* --- Certificates --- */
#define WOLFSSL_CERT_GEN
#define WOLFSSL_CERT_REQ
#define WOLFSSL_CERT_EXT
#define WOLFSSL_ALT_NAMES
#define WOLFSSL_ALLOW_ENCODING_CA_FALSE
#define WOLFSSL_EKU_OID
#define WOLFSSL_KEY_GEN
#define WOLFSSL_DER_TO_PEM
#define WOLFSSL_PEM_TO_DER
#define WOLFSSL_ASN_TEMPLATE
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY

/* --- Off --- */
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_DES3
#define NO_PSK
#define NO_PWDBASED
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

/* --- Misc --- */
#define WOLFSSL_SMALL_STACK      /* 1 MB thread stacks on the console; keep big buffers on the heap */
#define WOLFSSL_ERROR_CODE_OPENSSL
#define HAVE_ERRNO_H
#define WOLFSSL_STATIC_RSA_KEY_SIZE_TEST_OFF

#endif
