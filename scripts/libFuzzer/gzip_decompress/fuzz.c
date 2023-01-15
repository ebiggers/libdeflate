#include <libdeflate.h>
#include <stdint.h>
#include <stdlib.h>

/* Fuzz gzip decompression. */
int LLVMFuzzerTestOneInput(const uint8_t *in, size_t insize)
{
	size_t outsize_avail = 3 * insize;
	uint8_t *out;
	struct libdeflate_decompressor *d;

	out = malloc(outsize_avail);

	d = libdeflate_alloc_decompressor();
	libdeflate_gzip_decompress(d, in, insize, out, outsize_avail, NULL);
	libdeflate_free_decompressor(d);
	free(out);
	return 0;
}
