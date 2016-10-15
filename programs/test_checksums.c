/*
 * test_checksums.c
 *
 * Verify that libdeflate's Adler-32 and CRC-32 functions produce the same
 * results as their zlib equivalents.
 */

#include <time.h>
#include <zlib.h>

#include "prog_util.h"

static unsigned int rng_seed;

static void
assertion_failed(const char *file, int line)
{
	fprintf(stderr, "Assertion failed at %s:%d\n", file, line);
	fprintf(stderr, "RNG seed was %u\n", rng_seed);
	abort();
}

#define ASSERT(expr) if (!(expr)) assertion_failed(__FILE__, __LINE__);

typedef u32 (*cksum_fn_t)(u32, const void *, size_t);

static u32
zlib_adler32(u32 adler, const void *buf, size_t len)
{
	return adler32(adler, buf, len);
}

static u32
zlib_crc32(u32 crc, const void *buf, size_t len)
{
	return crc32(crc, buf, len);
}

static u32
select_initial_crc(void)
{
	if (rand() & 1)
		return 0;
	return ((u32)rand() << 16) | rand();
}

static u32
select_initial_adler(void)
{
	if (rand() & 1)
		return 1;
	return ((u32)(rand() % 65521) << 16) | (rand() % 65521);
}

static void
test_initial_values(cksum_fn_t cksum, u32 expected)
{
	ASSERT(cksum(0, NULL, 0) == expected);
	if (cksum != zlib_adler32) /* broken */
		ASSERT(cksum(0, NULL, 1) == expected);
	ASSERT(cksum(0, NULL, 1234) == expected);
	ASSERT(cksum(1234, NULL, 0) == expected);
	ASSERT(cksum(1234, NULL, 1234) == expected);
}

static void
test_multipart(const u8 *buffer, unsigned size, const char *name,
	       cksum_fn_t cksum, u32 v, u32 expected)
{
	unsigned division = (size != 0) ? rand() % size : 0;
	v = cksum(v, buffer, division);
	v = cksum(v, buffer + division, size - division);
	if (v != expected) {
		fprintf(stderr, "%s checksum failed multipart test\n", name);
		ASSERT(0);
	}
}

static void
test_checksums(const void *buffer, unsigned size, const char *name,
	       cksum_fn_t cksum1, cksum_fn_t cksum2, u32 initial_value)
{
	u32 v1 = cksum1(initial_value, buffer, size);
	u32 v2 = cksum2(initial_value, buffer, size);

	if (v1 != v2) {
		fprintf(stderr, "%s checksum mismatch\n", name);
		fprintf(stderr, "initial_value=0x%08"PRIx32", buffer=%p, "
			"contents ", initial_value, buffer);
		for (unsigned i = 0; i < size; i++)
			fprintf(stderr, "%02x", ((const u8 *)buffer)[i]);
		fprintf(stderr, "\n");
		ASSERT(0);
	}

	if ((rand() & 15) == 0) {
		test_multipart(buffer, size, name, cksum1, initial_value, v1);
		test_multipart(buffer, size, name, cksum2, initial_value, v1);
	}
}

int
tmain(int argc, tchar *argv[])
{
	u8 buffer[256];

	rng_seed = time(NULL);
	srand(rng_seed);

	test_initial_values(libdeflate_adler32, 1);
	test_initial_values(zlib_adler32, 1);
	test_initial_values(libdeflate_crc32, 0);
	test_initial_values(zlib_crc32, 0);

	for (uint32_t i = 0; i < 50000; i++) {
		/* test different buffer sizes and alignments */
		int start = rand() % sizeof(buffer);
		int len = rand() % (sizeof(buffer) - start);

		for (int i = start; i < start + len; i++)
			buffer[i] = rand();

		test_checksums(&buffer[start], len, "Adler-32",
			       libdeflate_adler32, zlib_adler32,
			       select_initial_adler());

		test_checksums(&buffer[start], len, "CRC-32",
			       libdeflate_crc32, zlib_crc32,
			       select_initial_crc());
	}

	printf("Adler-32 and CRC-32 checksum tests passed!\n");

	return 0;
}
