#include <assert.h>
#include <libdeflate.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Fuzz the DEFLATE compression and decompression round trip. */
int LLVMFuzzerTestOneInput(const uint8_t *in, size_t insize)
{
	int level;
	bool use_bound;
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;
	size_t csize_avail;
	uint8_t *cbuf;
	uint8_t *decompressed;
	size_t csize;
	enum libdeflate_result res;

	if (insize < 2)
		return 0;

	level = in[0] % 13;
	use_bound = in[1] % 2;
	in += 2;
	insize -= 2;

	c = libdeflate_alloc_compressor(level);
	d = libdeflate_alloc_decompressor();

	csize_avail = use_bound ? libdeflate_deflate_compress_bound(c, insize) :
				  insize;
	cbuf = malloc(csize_avail);
	decompressed = malloc(insize);

	csize = libdeflate_deflate_compress(c, in, insize, cbuf, csize_avail);
	if (csize != 0) {
		res = libdeflate_deflate_decompress(d, cbuf, csize, decompressed,
						    insize, NULL);
		assert(res == LIBDEFLATE_SUCCESS);
		assert(memcmp(in, decompressed, insize) == 0);
	} else {
		assert(!use_bound);
	}

	libdeflate_free_compressor(c);
	libdeflate_free_decompressor(d);
	free(cbuf);
	free(decompressed);
	return 0;
}
