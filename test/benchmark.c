/*
 * benchmark.c - A compression testing and benchmark program.
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */


#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libdeflate.h>
#include <zlib.h>

static void
usage(FILE *fp)
{
	static const char * const str =
"Usage: benchmark [FILE...]\n"
"\n"
"A compression and decompression benchmark and testing program.\n"
"Benchmarks are run on each FILE specified, or stdin if no file is specified.\n"
"\n"
"Options:\n"
"  -s, --chunk-size=SIZE        chunk size\n"
"  -l, --level=LEVEL            compression level [0-9]\n"
"  -1                           fastest\n"
"  -9                           slowest\n"
"  -z, --zlib                   use zlib wrapper\n"
"  -g, --gzip                   use gzip wrapper\n"
"  -Y, --compress-with-libz     compress with libz, not libdeflate\n"
"  -Z, --decompress-with-libz   decompress with libz, not libdeflate\n"
"  -h, --help                   print this help\n"
	;

	fputs(str, fp);
}

static void
fatal_error(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	fprintf(stderr, "ERROR: ");
	vfprintf(stderr, fmt, va);
	fprintf(stderr, "\n");
	va_end(va);

	exit(1);
}

#define ASSERT(expr, fmt, ...)				\
{							\
	if (!(expr))					\
		fatal_error((fmt), ## __VA_ARGS__);	\
}

enum wrapper {
	NO_WRAPPER,
	ZLIB_WRAPPER,
	GZIP_WRAPPER,
};

struct compressor {
	void *private;
	size_t (*compress)(void *, const void *, size_t, void *, size_t);
	void (*free_private)(void *);
};

static size_t
libz_compress(void *private, const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail)
{
	z_stream *z = private;

	deflateReset(z);

	z->next_in = (void *)in;
	z->avail_in = in_nbytes;
	z->next_out = out;
	z->avail_out = out_nbytes_avail;

	if (deflate(z, Z_FINISH) != Z_STREAM_END)
		return 0;

	return out_nbytes_avail - z->avail_out;
}

static void
libz_free_compressor_private(void *private)
{
	deflateEnd((z_stream *)private);
	free(private);
}

static int
get_libz_window_bits(enum wrapper wrapper)
{
	const int windowBits = 15;
	switch (wrapper) {
	case NO_WRAPPER:
		return -windowBits;
	case ZLIB_WRAPPER:
		return windowBits;
	case GZIP_WRAPPER:
		return windowBits + 16;
	}
	return windowBits;
}

static void
compressor_init(struct compressor *c, int level,
		enum wrapper wrapper, bool use_libz)
{
	if (use_libz) {
		z_stream *z;
		int zstatus;

		z = malloc(sizeof(z_stream));
		ASSERT(z != NULL, "out of memory");

		c->private = z;

		z->next_in = NULL;
		z->avail_in = 0;
		z->zalloc = NULL;
		z->zfree = NULL;
		z->opaque = NULL;
		zstatus = deflateInit2(z, level, Z_DEFLATED,
				       get_libz_window_bits(wrapper),
				       8, Z_DEFAULT_STRATEGY);
		ASSERT(zstatus == Z_OK, "unable to initialize deflater");
		c->compress = libz_compress;
		c->free_private = libz_free_compressor_private;
	} else {
		c->private = deflate_alloc_compressor(level);
		ASSERT(c->private != NULL, "failed to allocate compressor");
		switch (wrapper) {
		case NO_WRAPPER:
			c->compress = (void *)deflate_compress;
			break;
		case ZLIB_WRAPPER:
			c->compress = (void *)zlib_compress;
			break;
		case GZIP_WRAPPER:
			c->compress = (void *)gzip_compress;
			break;
		}
		c->free_private = (void *)deflate_free_compressor;
	}
}

static size_t
do_compress(struct compressor *c, const void *in, size_t in_nbytes,
	    void *out, size_t out_nbytes_avail)
{
	return (*c->compress)(c->private, in, in_nbytes, out, out_nbytes_avail);
}

static void
compressor_destroy(struct compressor *c)
{
	(*c->free_private)(c->private);
}

struct decompressor {
	void *private;
	bool (*decompress)(void *, const void *, size_t, void *, size_t);
	void (*free_private)(void *);
};

static bool
libz_decompress(void *private, const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes)
{
	z_stream *z = private;

	inflateReset(z);

	z->next_in = (void *)in;
	z->avail_in = in_nbytes;
	z->next_out = out;
	z->avail_out = out_nbytes;

	return (inflate(z, Z_FINISH) == Z_STREAM_END && z->avail_out == 0);
}

static void
libz_free_decompressor_private(void *private)
{
	inflateEnd((z_stream *)private);
	free(private);
}

static void
decompressor_init(struct decompressor *d, enum wrapper wrapper, bool use_libz)
{
	if (use_libz) {
		z_stream *z;
		int zstatus;

		z = malloc(sizeof(z_stream));
		ASSERT(z != NULL, "out of memory");

		d->private = z;

		z->next_in = NULL;
		z->avail_in = 0;
		z->zalloc = NULL;
		z->zfree = NULL;
		z->opaque = NULL;
		zstatus = inflateInit2(z, get_libz_window_bits(wrapper));
		ASSERT(zstatus == Z_OK, "failed to initialize inflater");

		d->decompress = libz_decompress;
		d->free_private = libz_free_decompressor_private;
	} else {
		d->private = deflate_alloc_decompressor();
		ASSERT(d->private != NULL, "out of memory");
		switch (wrapper) {
		case NO_WRAPPER:
			d->decompress = (void *)deflate_decompress;
			break;
		case ZLIB_WRAPPER:
			d->decompress = (void *)zlib_decompress;
			break;
		case GZIP_WRAPPER:
			d->decompress = (void *)gzip_decompress;
			break;
		}
		d->free_private = (void *)deflate_free_decompressor;
	}
}

static bool
do_decompress(struct decompressor *d, const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes)
{
	return (*d->decompress)(d->private, in, in_nbytes, out, out_nbytes);
}

static void
decompressor_destroy(struct decompressor *d)
{
	(*d->free_private)(d->private);
}

#define TIME_UNIT_PER_MS 1000000
static uint64_t
current_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)1000000000 * ts.tv_sec) + ts.tv_nsec;
}

static void
do_benchmark(int fd, char *ubuf1, char *ubuf2,
	     char *cbuf, uint32_t max_chunk_size,
	     struct compressor *c, struct decompressor *d)
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
		uint64_t start_time;
		bool ok;

		/* Read the next chunk of data.  */
		do {
			bytes_read = read(fd, p, ubuf1 + max_chunk_size - p);
			ASSERT(bytes_read >= 0, "read error");
			p += bytes_read;
		} while (bytes_read != 0 && p != ubuf1 + max_chunk_size);

		usize = p - ubuf1;

		if (usize == 0)  /* End of file?  */
			break;

		/* Compress the chunk of data.  */
		usize_total += usize;
		start_time = current_time();
		csize = do_compress(c, ubuf1, usize, cbuf, usize - 1);
		compress_time_total += current_time() - start_time;

		if (csize) {
			/* Successfully compressed the chunk of data.  */
			csize_total += csize;

			/* Decompress the data we just compressed and compare
			 * the result with the original.  */
			start_time = current_time();

			ok = do_decompress(d, cbuf, csize, ubuf2, usize);

			decompress_time_total += current_time() - start_time;

			ASSERT(ok, "failed to decompress data");

			ASSERT(!memcmp(ubuf1, ubuf2, usize),
			       "data did not decompress to original");
		} else {
			/* Chunk of data did not compress to less than its
			 * original size.  */
			csize_total += usize;
		}
	}


	if (usize_total == 0) {
		printf("\tEmpty input.\n");
		return;
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
}

static const char *const optstring = "s:l:0123456789gzYZh";
static const struct option longopts[] = {
	{"chunk-size",           required_argument, NULL, 's'},
	{"level",                required_argument, NULL, 'l'},
	{"zlib",                 no_argument,       NULL, 'z'},
	{"gzip",                 no_argument,       NULL, 'g'},
	{"compress-with-libz",   no_argument,       NULL, 'Y'},
	{"decompress-with-libz", no_argument,       NULL, 'Z'},
	{"help",                 no_argument,       NULL, 'h'},
	{NULL, 0, NULL, 0},
};

int
main(int argc, char **argv)
{
	uint32_t chunk_size = 32768;
	int level = 6;
	enum wrapper wrapper = NO_WRAPPER;
	bool compress_with_libz = false;
	bool decompress_with_libz = false;
	char *ubuf1;
	char *ubuf2;
	char *cbuf;
	struct compressor c;
	struct decompressor d;
	int opt_char;

	while ((opt_char =
		getopt_long(argc, argv, optstring, longopts, NULL)) != -1)
	{
		switch (opt_char) {
		case 's':
			chunk_size = strtoul(optarg, NULL, 10);
			break;
		case 'l':
			level = strtoul(optarg, NULL, 10);
			break;
		case '1' ... '9':
			level = opt_char - '0';
			break;
		case 'z':
			wrapper = ZLIB_WRAPPER;
			break;
		case 'g':
			wrapper = GZIP_WRAPPER;
			break;
		case 'Y':
			compress_with_libz = true;
			break;
		case 'Z':
			decompress_with_libz = true;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 1;
		}
	}

	argc -= optind;
	argv += optind;

	printf("Benchmarking DEFLATE compression:\n");
	printf("\tCompression level: %d\n", level);
	printf("\tChunk size: %"PRIu32"\n", chunk_size);
	printf("\tWrapper: %s\n",
	       wrapper == NO_WRAPPER ? "None" :
	       wrapper == ZLIB_WRAPPER ? "zlib" : "gzip");
	printf("\tCompression engine: %s\n",
	       compress_with_libz ? "zlib" : "libdeflate");
	printf("\tDecompression engine: %s\n",
	       decompress_with_libz ? "zlib" : "libdeflate");

	ubuf1 = malloc(chunk_size);
	ubuf2 = malloc(chunk_size);
	cbuf = malloc(chunk_size - 1);

	ASSERT(ubuf1 != NULL && ubuf2 != NULL && cbuf != NULL,
	       "out of memory");

	compressor_init(&c, level, wrapper, compress_with_libz);
	decompressor_init(&d, wrapper, decompress_with_libz);

	if (argc == 0) {
		printf("Reading from stdin...\n");
		do_benchmark(STDIN_FILENO, ubuf1, ubuf2,
			     cbuf, chunk_size, &c, &d);
	} else {
		for (int i = 0; i < argc; i++) {
			printf("Processing \"%s\"...\n", argv[i]);
			int fd = open(argv[i], O_RDONLY);
			ASSERT(fd >= 0,
			       "Can't open \"%s\" for reading: %s\n",
			       argv[i], strerror(errno));
			do_benchmark(fd, ubuf1, ubuf2, cbuf, chunk_size, &c, &d);
			close(fd);
		}
	}

	decompressor_destroy(&d);
	compressor_destroy(&c);
	free(cbuf);
	free(ubuf2);
	free(ubuf1);
	return 0;
}
