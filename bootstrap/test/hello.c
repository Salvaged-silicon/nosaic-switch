/*
 * The M1 gate: proof that a toolchain produces binaries that actually run on
 * the target it claims.
 *
 * It reports what it observes rather than just printing "hello", because a
 * toolchain misconfigured for the wrong endianness or word size still produces
 * a program that runs and prints something. Those are exactly the mistakes
 * that otherwise surface much later, on hardware, as inexplicable corruption.
 */
#include <stdio.h>
#include <stdint.h>

int main(void)
{
	const uint32_t probe = 0x01020304u;
	const uint8_t first = *(const uint8_t *)&probe;
	const char *endian = (first == 0x01) ? "big"
			   : (first == 0x04) ? "little"
					     : "unknown";

	printf("nosaic bits=%zu endian=%s ptr=%zu long=%zu\n",
	       sizeof(void *) * 8, endian, sizeof(void *), sizeof(long));
	return 0;
}
