/* Runs wolfCrypt's own test suite (third_party/wolfssl/wolfcrypt/test/test.c) on the host. */
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfcrypt/test/test.h>

int main(void)
{
	return wolfcrypt_test(NULL);
}
