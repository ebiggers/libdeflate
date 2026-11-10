/*
 * test_size_edge_cases.c
 *
 * Copyright 2026 Google LLC
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Test various buffer size edge cases:
 *   - (NULL, 0) input and output buffers
 *   - *_compress_bound() with values near SIZE_MAX
 *   - buffer sizes larger than PTRDIFF_MAX
 */

#include "test_util.h"

static const struct {
	size_t (*compress_bound)(struct libdeflate_compressor *compressor,
				 size_t in_nbytes);
	size_t (*compress)(struct libdeflate_compressor *compressor,
			   const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail);
	enum libdeflate_result (*decompress)(
			struct libdeflate_decompressor *decompressor,
			const void *in, size_t in_nbytes,
			void *out, size_t out_nbytes_avail,
			size_t *actual_out_nbytes_ret);
	enum libdeflate_result (*decompress_ex)(
			struct libdeflate_decompressor *decompressor,
			const void *in, size_t in_nbytes,
			void *out, size_t out_nbytes_avail,
			size_t *actual_in_nbytes_ret,
			size_t *actual_out_nbytes_ret);
} codecs[] = {
	{
		.compress_bound = libdeflate_deflate_compress_bound,
		.compress = libdeflate_deflate_compress,
		.decompress = libdeflate_deflate_decompress,
		.decompress_ex = libdeflate_deflate_decompress_ex,
	}, {
		.compress_bound = libdeflate_zlib_compress_bound,
		.compress = libdeflate_zlib_compress,
		.decompress = libdeflate_zlib_decompress,
		.decompress_ex = libdeflate_zlib_decompress_ex,
	}, {
		.compress_bound = libdeflate_gzip_compress_bound,
		.compress = libdeflate_gzip_compress,
		.decompress = libdeflate_gzip_decompress,
		.decompress_ex = libdeflate_gzip_decompress_ex,
	}
};

/*
 * Test that passing (NULL, 0) as the input and/or output buffer to the
 * compression and decompression functions works as expected and doesn't
 * execute 'NULL + 0' undefined behavior.
 */
static void
test_null_empty_buffers(struct libdeflate_compressor *c,
			struct libdeflate_decompressor *d)
{
	const u8 dummy_in = 0x42;
	u8 compressed[64];
	u8 decompressed[16];
	size_t i;

	for (i = 0; i < ARRAY_LEN(codecs); i++) {
		size_t bound, csize;
		size_t actual_in_nbytes, actual_out_nbytes;

		bound = codecs[i].compress_bound(c, 0);
		ASSERT(bound > 0 && bound <= sizeof(compressed));
		ASSERT(codecs[i].compress_bound(NULL, 0) == bound);

		/* Compress with (NULL, 0) output buffer */
		ASSERT(codecs[i].compress(c, NULL, 0, NULL, 0) == 0);
		ASSERT(codecs[i].compress(c, &dummy_in, 1, NULL, 0) == 0);

		/* Compress with zero-length, non-NULL output buffer */
		ASSERT(codecs[i].compress(c, &dummy_in, 0, decompressed, 0) == 0);

		/* Compress (NULL, 0) input buffer into valid output buffer */
		csize = codecs[i].compress(c, NULL, 0, compressed, bound);
		ASSERT(csize > 0 && csize <= bound);

		/* Decompress (NULL, 0) input buffer */
		ASSERT(codecs[i].decompress(d, NULL, 0, NULL, 0, NULL) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress(d, NULL, 0, decompressed,
					    sizeof(decompressed),
					    &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress_ex(d, NULL, 0, NULL, 0,
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);

		/* Decompress empty stream into (NULL, 0) output buffer */
		ASSERT(codecs[i].decompress(d, compressed, csize,
					    NULL, 0, NULL) ==
		       LIBDEFLATE_SUCCESS);

		actual_out_nbytes = 123;
		ASSERT(codecs[i].decompress(d, compressed, csize, NULL, 0,
					    &actual_out_nbytes) ==
		       LIBDEFLATE_SUCCESS);
		ASSERT(actual_out_nbytes == 0);

		actual_in_nbytes = actual_out_nbytes = 123;
		ASSERT(codecs[i].decompress_ex(d, compressed, csize, NULL, 0,
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_SUCCESS);
		ASSERT(actual_in_nbytes == csize);
		ASSERT(actual_out_nbytes == 0);

		/* Decompress non-empty stream into (NULL, 0) output buffer */
		csize = codecs[i].compress(c, &dummy_in, 1,
					   compressed, sizeof(compressed));
		ASSERT(csize > 0);
		ASSERT(codecs[i].decompress(d, compressed, csize,
					    NULL, 0, NULL) ==
		       LIBDEFLATE_INSUFFICIENT_SPACE);
		ASSERT(codecs[i].decompress(d, compressed, csize, NULL, 0,
					    &actual_out_nbytes) ==
		       LIBDEFLATE_INSUFFICIENT_SPACE);
		ASSERT(codecs[i].decompress_ex(d, compressed, csize, NULL, 0,
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_INSUFFICIENT_SPACE);
	}
}

/*
 * Test that *_compress_bound() doesn't wrap around when given values near
 * SIZE_MAX.
 */
static void
test_compress_bound_near_size_max(struct libdeflate_compressor *c)
{
	const size_t step = (SIZE_MAX - (size_t)PTRDIFF_MAX) / 1024;
	size_t i, k, delta;

	for (i = 0; i < ARRAY_LEN(codecs); i++) {
		size_t bound = codecs[i].compress_bound(c, PTRDIFF_MAX);
		size_t prev_bound;

		/*
		 * The bound for PTRDIFF_MAX must not be saturated, since the
		 * real bound is far below SIZE_MAX.  This catches an
		 * implementation that just returns SIZE_MAX for large values.
		 */
		ASSERT(bound > (size_t)PTRDIFF_MAX && bound < SIZE_MAX);
		ASSERT(codecs[i].compress_bound(NULL, PTRDIFF_MAX) == bound);

		/*
		 * Sweep from PTRDIFF_MAX to SIZE_MAX.  The bound must never be
		 * less than the input size, and it must be monotonic.  Either
		 * property would be violated by integer wraparound.
		 */
		prev_bound = bound;
		for (k = 1; k <= 1024; k++) {
			size_t in_nbytes = (size_t)PTRDIFF_MAX + (k * step);

			bound = codecs[i].compress_bound(c, in_nbytes);
			ASSERT(bound >= in_nbytes);
			ASSERT(bound >= prev_bound);
			ASSERT(codecs[i].compress_bound(NULL, in_nbytes) ==
			       bound);
			prev_bound = bound;
		}

		/* Values very close to SIZE_MAX must saturate to SIZE_MAX. */
		for (delta = 0; delta < 20000; delta += (delta < 64 ? 1 : 37)) {
			size_t in_nbytes = SIZE_MAX - delta;

			ASSERT(codecs[i].compress_bound(c, in_nbytes) ==
			       SIZE_MAX);
			ASSERT(codecs[i].compress_bound(NULL, in_nbytes) ==
			       SIZE_MAX);
		}
	}
}

/*
 * Test that the compression and decompression functions reject buffers larger
 * than PTRDIFF_MAX.
 *
 * Note: compression with out_nbytes_avail > PTRDIFF_MAX is not tested, since
 * the library just clamps it to PTRDIFF_MAX.  The only way to test that would
 * be to lie about the output buffer size, which would itself be undefined.
 */
static void
test_buffers_larger_than_ptrdiff_max(struct libdeflate_compressor *c,
				     struct libdeflate_decompressor *d)
{
	const u8 dummy_in = 0x42;
	u8 compressed[64];
	u8 decompressed[16];
	size_t i;

	for (i = 0; i < ARRAY_LEN(codecs); i++) {
		size_t csize;
		size_t actual_in_nbytes;
		size_t actual_out_nbytes;

		csize = codecs[i].compress(c, &dummy_in, 1,
					   compressed, sizeof(compressed));
		ASSERT(csize > 0);

		ASSERT(codecs[i].compress(c, &dummy_in,
					  (size_t)PTRDIFF_MAX + 1,
					  compressed, sizeof(compressed)) == 0);
		ASSERT(codecs[i].compress(c, &dummy_in, SIZE_MAX,
					  compressed, sizeof(compressed)) == 0);

		ASSERT(codecs[i].decompress(d, compressed,
					    (size_t)PTRDIFF_MAX + 1,
					    decompressed, sizeof(decompressed),
					    &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress(d, compressed, SIZE_MAX,
					    decompressed, sizeof(decompressed),
					    &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress_ex(d, compressed,
					       (size_t)PTRDIFF_MAX + 1,
					       decompressed,
					       sizeof(decompressed),
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress_ex(d, compressed, SIZE_MAX,
					       decompressed,
					       sizeof(decompressed),
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);

		ASSERT(codecs[i].decompress(d, compressed, csize,
					    decompressed,
					    (size_t)PTRDIFF_MAX + 1, NULL) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress(d, compressed, csize,
					    decompressed, SIZE_MAX, NULL) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress_ex(d, compressed, csize,
					       decompressed,
					       (size_t)PTRDIFF_MAX + 1,
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
		ASSERT(codecs[i].decompress_ex(d, compressed, csize,
					       decompressed, SIZE_MAX,
					       &actual_in_nbytes,
					       &actual_out_nbytes) ==
		       LIBDEFLATE_BAD_DATA);
	}
}

int
tmain(int argc, tchar *argv[])
{
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;

	begin_program(argv);

	c = libdeflate_alloc_compressor(6);
	ASSERT(c != NULL);
	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	test_null_empty_buffers(c, d);
	test_compress_bound_near_size_max(c);
	test_buffers_larger_than_ptrdiff_max(c, d);

	libdeflate_free_compressor(c);
	libdeflate_free_decompressor(d);
	return 0;
}
