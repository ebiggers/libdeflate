/*
 * test_zlib.c
 *
 * Verify that libdeflate_zlib_decompress and libdeflate_zlib_decompress_ex can
 * correctly decompress the results of libdeflate_zlib_compress.  Also checks
 * whether decompression correctly handles additional trailing bytes in the
 * compressed buffer.
 */

#include <stdlib.h>

#include "test_util.h"

static void
test_decompress(u8 const* compressed, size_t compressed_nbytes,
		u8 const* expected, size_t expected_nbytes)
{
	struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	size_t decompressed_nbytes = expected_nbytes;
	u8 *decompressed = xmalloc(decompressed_nbytes);

	size_t actual_decompressed_nbytes = 0;
	enum libdeflate_result res =
		libdeflate_zlib_decompress(d, compressed, compressed_nbytes,
					   decompressed, decompressed_nbytes,
					   &actual_decompressed_nbytes);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_decompressed_nbytes == expected_nbytes);
	ASSERT(memcmp(expected, decompressed, expected_nbytes) == 0);

	free(decompressed);
	libdeflate_free_decompressor(d);
}

static void
test_decompress_ex(u8 const* compressed, size_t compressed_nbytes,
		   size_t expected_actual_compressed_nbytes,
		   u8 const* expected, size_t expected_nbytes)
{
	struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	size_t decompressed_nbytes = expected_nbytes;
	u8 *decompressed = xmalloc(decompressed_nbytes);

	size_t actual_compressed_nbytes = 0;
	size_t actual_decompressed_nbytes = 0;
	enum libdeflate_result res =
		libdeflate_zlib_decompress_ex(d, compressed, compressed_nbytes,
					      decompressed, decompressed_nbytes,
					      &actual_compressed_nbytes,
					      &actual_decompressed_nbytes);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_compressed_nbytes == expected_actual_compressed_nbytes);
	ASSERT(actual_decompressed_nbytes == expected_nbytes);
	ASSERT(memcmp(expected, decompressed, expected_nbytes) == 0);

	free(decompressed);
	libdeflate_free_decompressor(d);
}

int
tmain(int argc, tchar *argv[])
{
	program_invocation_name = get_filename(argv[0]);

	size_t original_nbytes = 32768;
	u8 *original = xmalloc(original_nbytes);

	/* Prepare some dummy data to compress */
	for (size_t i = 0; i < original_nbytes; ++i) {
		original[i] = (i % 123) + (i % 1023);
	}

	size_t compressed_nbytes_total = 32768;
	u8 *compressed = xmalloc(compressed_nbytes_total);
	memset(compressed, 0x00, compressed_nbytes_total);

	/*
	 * Don't use the full buffer for compressed data, because we want to
	 * test whether decompression can deal with additional trailing bytes.
	 */
	size_t compressed_nbytes_avail = 30000;
	ASSERT(compressed_nbytes_avail < compressed_nbytes_total);

	struct libdeflate_compressor* c = libdeflate_alloc_compressor(6);
	ASSERT(c != NULL);
	size_t compressed_nbytes =
		libdeflate_zlib_compress(c, original, original_nbytes,
					 compressed, compressed_nbytes_avail);
	ASSERT(compressed_nbytes > 0);
	libdeflate_free_compressor(c);

	test_decompress(compressed, compressed_nbytes, original, original_nbytes);
	test_decompress(compressed, compressed_nbytes_total, original, original_nbytes);
	test_decompress_ex(compressed, compressed_nbytes_total, compressed_nbytes,
			   original, original_nbytes);

	printf("libdeflate_zlib_* tests passed!\n");

	free(compressed);
	free(original);
	return 0;
}
