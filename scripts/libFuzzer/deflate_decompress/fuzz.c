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

/* Fuzz DEFLATE decompression. */
int LLVMFuzzerTestOneInput(const uint8_t *in, size_t insize)
{
	size_t outsize_avail = 3 * insize;
	uint8_t *cbuf_start, *cbuf_end, *cbuf;
	uint8_t *dbuf_start, *dbuf_end, *dbuf;
	struct libdeflate_decompressor *d;

	/* Use guard pages to make all input/output buffer overflows segfault */

	alloc_guarded_buffer(insize, &cbuf_start, &cbuf_end);
	cbuf = cbuf_end - insize;
	memcpy(cbuf, in, insize);

	alloc_guarded_buffer(outsize_avail, &dbuf_start, &dbuf_end);
	dbuf = dbuf_end - outsize_avail;

	d = libdeflate_alloc_decompressor();
	libdeflate_deflate_decompress(d, cbuf, insize, dbuf, outsize_avail,
				      NULL);
	libdeflate_free_decompressor(d);
	free_guarded_buffer(cbuf_start, cbuf_end);
	free_guarded_buffer(dbuf_start, dbuf_end);
	return 0;
}
