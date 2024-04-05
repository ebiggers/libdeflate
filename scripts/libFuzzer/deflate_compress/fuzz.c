#include <assert.h>
#include <libdeflate.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void
alloc_guarded_buffer(size_t size, uint8_t **start_ret, uint8_t **end_ret)
{
	const size_t pagesize = sysconf(_SC_PAGESIZE);
	const size_t nr_pages = (size + pagesize - 1) / pagesize;
	uint8_t *base_addr, *start, *end;

	/* Allocate buffer and guard pages. */
	base_addr = mmap(NULL, (nr_pages + 2) * pagesize, PROT_READ|PROT_WRITE,
			 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	assert(base_addr != (uint8_t *)MAP_FAILED);
	start = base_addr + pagesize;
	end = start + (nr_pages * pagesize);

	/* Unmap the guard pages. */
	munmap(base_addr, pagesize);
	munmap(end, pagesize);

	*start_ret = start;
	*end_ret = end;
}

static void
free_guarded_buffer(uint8_t *start, uint8_t *end)
{
	munmap(start, end - start);
}

/* Fuzz the DEFLATE compression and decompression round trip. */
int LLVMFuzzerTestOneInput(const uint8_t *in, size_t insize)
{
	int level;
	bool use_bound;
	struct libdeflate_compressor *c;
	struct libdeflate_decompressor *d;
	size_t csize_avail;
	uint8_t *ubuf_start, *ubuf_end, *ubuf;
	uint8_t *cbuf_start, *cbuf_end, *cbuf;
	uint8_t *dbuf_start, *dbuf_end, *dbuf;
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

	/* Use guard pages to make all input/output buffer overflows segfault */

	alloc_guarded_buffer(insize, &ubuf_start, &ubuf_end);
	ubuf = ubuf_end - insize;
	memcpy(ubuf, in, insize);

	csize_avail = use_bound ? libdeflate_deflate_compress_bound(c, insize) :
				  insize;
	alloc_guarded_buffer(csize_avail, &cbuf_start, &cbuf_end);
	cbuf = cbuf_end - csize_avail;

	alloc_guarded_buffer(insize, &dbuf_start, &dbuf_end);
	dbuf = dbuf_end - insize;

	csize = libdeflate_deflate_compress(c, ubuf, insize, cbuf, csize_avail);
	if (csize != 0) {
		assert(csize <= csize_avail);
		memmove(cbuf_end - csize, cbuf, csize);
		res = libdeflate_deflate_decompress(d, cbuf_end - csize, csize,
						    dbuf, insize, NULL);
		assert(res == LIBDEFLATE_SUCCESS);
		assert(memcmp(in, dbuf, insize) == 0);
	} else {
		assert(!use_bound);
	}

	libdeflate_free_compressor(c);
	libdeflate_free_decompressor(d);
	free_guarded_buffer(ubuf_start, ubuf_end);
	free_guarded_buffer(cbuf_start, cbuf_end);
	free_guarded_buffer(dbuf_start, dbuf_end);
	return 0;
}
