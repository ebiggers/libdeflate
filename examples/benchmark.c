/*
 * benchmark.c - A compression testing and benchmark program.
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#include <libdeflate.h>

#ifdef __WIN32__
#  include <windows.h>
#else
#  define _FILE_OFFSET_BITS 64
#  define O_BINARY 0
#  define _POSIX_C_SOURCE 199309L
#  include <time.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t
current_time(void)
{
#ifdef __WIN32__
#  define TIME_UNIT_PER_MS 10000
	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);
	return time.QuadPart;
#else
#  define TIME_UNIT_PER_MS 1000000
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (1000000000ULL * ts.tv_sec) + ts.tv_nsec;
#endif
}

static int
do_benchmark(int fd, char *ubuf1, char *ubuf2,
	     char *cbuf, uint32_t max_chunk_size,
	     struct deflate_compressor *compressor,
	     struct deflate_decompressor *decompressor)
{
	uint64_t usize_total = 0;
	uint64_t csize_total = 0;
	uint64_t compress_time_total = 0;
	uint64_t decompress_time_total = 0;

	for (;;) {
		char *p = ubuf1;
		ssize_t bytes_read;
		size_t usize;
		size_t csize;
		bool ok;
		uint64_t start_time;

		/* Read the next chunk of data.  */
		do {
			bytes_read = read(fd, p, ubuf1 + max_chunk_size - p);
			if (bytes_read < 0) {
				fprintf(stderr, "ERROR: Read error: %s\n",
					strerror(errno));
				return 1;
			}
			p += bytes_read;
		} while (bytes_read != 0 && p != ubuf1 + max_chunk_size);

		usize = p - ubuf1;

		if (usize == 0)  /* End of file?  */
			break;

		/* Compress the chunk of data.  */
		usize_total += usize;
		start_time = current_time();
		csize = deflate_compress(compressor, ubuf1, usize,
					 cbuf, usize - 1);
		compress_time_total += current_time() - start_time;

		if (csize) {
			/* Successfully compressed the chunk of data.  */
			csize_total += csize;

			/* Decompress the data we just compressed and compare
			 * the result with the original.  */
			start_time = current_time();
			ok = deflate_decompress(decompressor, cbuf, csize,
						ubuf2, usize);
			decompress_time_total += current_time() - start_time;
			if (!ok) {
				fprintf(stderr, "ERROR: Failed to "
					"decompress data\n");
				return 1;
			}

			if (memcmp(ubuf1, ubuf2, usize)) {
				fprintf(stderr, "ERROR: Data did not "
					"decompress to original\n");
				return 1;
			}
		} else {
			/* Chunk of data did not compress to less than its
			 * original size.  */
			csize_total += usize;
		}
	}


	if (usize_total == 0) {
		printf("\tEmpty input.\n");
		return 0;
	}

	if (compress_time_total == 0)
		compress_time_total++;
	if (decompress_time_total == 0)
		decompress_time_total++;

	printf("\tCompressed %"PRIu64 " => %"PRIu64" bytes (%u.%u%%)\n",
	       usize_total, csize_total,
	       (unsigned int)(csize_total * 100 / usize_total),
	       (unsigned int)(csize_total * 100000 / usize_total % 1000));
	printf("\tCompression time: %"PRIu64" ms (%"PRIu64" MB/s)\n",
	       compress_time_total / TIME_UNIT_PER_MS,
	       1000 * usize_total / compress_time_total);
	printf("\tDecompression time: %"PRIu64" ms (%"PRIu64" MB/s)\n",
	       decompress_time_total / TIME_UNIT_PER_MS,
	       1000 * usize_total / decompress_time_total);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *filename;
	uint32_t chunk_size = 32768;
	unsigned int compression_level = 6;
	char *ubuf1 = NULL;
	char *ubuf2 = NULL;
	char *cbuf = NULL;
	struct deflate_compressor *compressor = NULL;
	struct deflate_decompressor *decompressor = NULL;
	int fd = -1;
	int ret;

	if (argc < 2 || argc > 5) {
		fprintf(stderr, "Usage: %s FILE [CHUNK_SIZE [LEVEL]]]\n", argv[0]);
		ret = 2;
		goto out;
	}

	filename = argv[1];

	if (argc >= 3)
		chunk_size = strtoul(argv[2], NULL, 10);

	if (argc >= 4)
		compression_level = strtoul(argv[3], NULL, 10);

	printf("DEFLATE compression with %"PRIu32" byte chunks (level %u)\n",
	       chunk_size, compression_level);

	compressor = deflate_alloc_compressor(compression_level);
	if (!compressor) {
		fprintf(stderr, "ERROR: Failed to create compressor\n");
		ret = 1;
		goto out;
	}

	decompressor = deflate_alloc_decompressor();
	if (!decompressor) {
		fprintf(stderr, "ERROR: Failed to create decompressor\n");
		ret = 1;
		goto out;
	}

	ubuf1 = malloc(chunk_size);
	ubuf2 = malloc(chunk_size);
	cbuf = malloc(chunk_size - 1);

	if (!ubuf1 || !ubuf2 || !cbuf) {
		fprintf(stderr, "ERROR: Insufficient memory\n");
		ret = 1;
		goto out;
	}

	fd = open(filename, O_RDONLY | O_BINARY);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Can't open \"%s\" for reading: %s\n",
			filename, strerror(errno));
		ret = 1;
		goto out;
	}

	ret = do_benchmark(fd, ubuf1, ubuf2, cbuf, chunk_size,
			   compressor, decompressor);
out:
	close(fd);
	free(cbuf);
	free(ubuf2);
	free(ubuf1);
	deflate_free_decompressor(decompressor);
	deflate_free_compressor(compressor);
	return ret;
}
