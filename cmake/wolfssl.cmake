# wolfSSL from third_party/wolfssl (pinned upstream tag), built with our own settings header
# (third_party/wolfssl-config/user_settings.h) for both the host and the console. Upstream's CMake is skipped: its
# probes don't suit the console, and WOLFSSL_USER_SETTINGS replaces all of its options anyway.
set(XST_WOLFSSL_DIR "${CMAKE_SOURCE_DIR}/third_party/wolfssl")
set(XST_WOLFSSL_CONFIG "${CMAKE_SOURCE_DIR}/third_party/wolfssl-config")

file(GLOB xst_wolfcrypt_sources "${XST_WOLFSSL_DIR}/wolfcrypt/src/*.c")
# misc.c is included by the other sources; evp.c by ssl.c; the rest are FIPS or async builds.
list(FILTER xst_wolfcrypt_sources EXCLUDE REGEX
	"/(evp|misc|selftest|fips|fips_test|wolfcrypt_first|wolfcrypt_last|async)\\.c$")
file(GLOB xst_wolfssl_sources "${XST_WOLFSSL_DIR}/src/*.c")
# These are #included by ssl.c.
list(FILTER xst_wolfssl_sources EXCLUDE REGEX
	"/(bio|conf|pk|pk_ec|pk_rsa|sniffer|x509|x509_str|ssl_[a-z0-9_]+)\\.c$")

add_library(xst_wolfssl STATIC ${xst_wolfcrypt_sources} ${xst_wolfssl_sources})
target_include_directories(xst_wolfssl SYSTEM PUBLIC "${XST_WOLFSSL_DIR}" "${XST_WOLFSSL_CONFIG}")
target_compile_definitions(xst_wolfssl PUBLIC WOLFSSL_USER_SETTINGS)
if(XSDK)
	target_sources(xst_wolfssl PRIVATE "${XST_WOLFSSL_CONFIG}/seed_xsdk.c")
	# The console's newlib is strict about some of wolfSSL's casts; keep warnings visible but not fatal here.
	target_compile_options(xst_wolfssl PRIVATE -w)
else()
	target_compile_options(xst_wolfssl PRIVATE -w)
	target_link_libraries(xst_wolfssl PUBLIC m)
endif()

# wolfCrypt's own test suite, as a host program and, on the console, as a title.
add_library(xst_wolfcrypt_test OBJECT "${XST_WOLFSSL_DIR}/wolfcrypt/test/test.c")
target_link_libraries(xst_wolfcrypt_test PUBLIC xst_wolfssl)
target_compile_definitions(xst_wolfcrypt_test PRIVATE NO_MAIN_DRIVER)
target_compile_options(xst_wolfcrypt_test PRIVATE -w)
if(XSDK)
	# xsdk's GCC defines __unix__, which turns on the RNG lock tests that need pipe(); the console has none.
	target_compile_options(xst_wolfcrypt_test PRIVATE -U__unix__ -U__unix)
	# The suite's keys and certificates built in, not read from ./certs (which isn't beside the .xex).
	target_compile_definitions(xst_wolfcrypt_test PRIVATE NO_FILESYSTEM USE_CERT_BUFFERS_2048 USE_CERT_BUFFERS_256
		USE_CERT_BUFFERS_25519)
endif()
