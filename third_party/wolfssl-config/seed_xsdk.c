/* wolfSSL's RNG seed on the console: xsdk's getentropy, which reads the kernel's RNG. In Xenia it returns the same
 * bytes every time, so keys made there are for testing only (xsdk: mbedtls-xenon docs/README-xsdk.md). */
#include <stddef.h>
#include <unistd.h>

int xst_wolfssl_seed(unsigned char *output, unsigned int size)
{
	/* getentropy takes at most 256 bytes a call. */
	while (size > 0) {
		const unsigned int chunk = size > 256 ? 256 : size;
		if (getentropy(output, chunk) != 0)
			return -1;
		output += chunk;
		size -= chunk;
	}
	return 0;
}
