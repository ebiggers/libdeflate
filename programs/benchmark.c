/*
 * benchmark.c - a compression testing and benchmark program
 *
 * Copyright 2016 Eric Biggers
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

#include <zlib.h> /* for comparison purposes */

#include "prog_util.h"

static const tchar *const optstring = T("1::2::3::4::5::6::7::8::9::ghs:VYZz");

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %"TS" [-LVL] [-ghVYZz] [-s SIZE] [FILE]...\n"
"Benchmark DEFLATE compression and decompression on the specified FILEs.\n"
"\n"
"Options:\n"
"  -1        fastest (worst) compression\n"
"  -6        medium compression (default)\n"
"  -12       slowest (best) compression\n"
"  -g        use gzip wrapper\n"
"  -h        print this help\n"
"  -s SIZE   chunk size\n"
"  -V        show version and legal information\n"
"  -Y        compress with libz, not libdeflate\n"
"  -Z        decompress with libz, not libdeflate\n"
"  -z        use zlib wrapper\n",
	program_invocation_name);
}

static void
show_version(void)
{
	printf(
"libdeflate compression benchmark program v" LIBDEFLATE_VERSION_STRING "\n"
"Copyright 2016 Eric Biggers\n"
"\n"
"This program is free software which may be modified and/or redistributed\n"
"under the terms of the MIT license.  There is NO WARRANTY, to the extent\n"
"permitted by law.  See the COPYING file for details.\n"
	);
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

struct decompressor {
	void *private;
	bool (*decompress)(void *, const void *, size_t, void *, size_t);
	void (*free_private)(void *);
};

static int
get_libz_window_bits(enum wrapper wrapper)
{
	const int windowBits = 15;
	switch (wrapper) {
	case ZLIB_WRAPPER:
		return windowBits;
	case GZIP_WRAPPER:
		return windowBits + 16;
	default:
		return -windowBits;
	}
}

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

static bool
libz_decompress(void *private, const void *in, size_t in_nbytes,
		void *out, size_t out_nbytes_avail)
{
	z_stream *z = private;

	inflateReset(z);

	z->next_in = (void *)in;
	z->avail_in = in_nbytes;
	z->next_out = out;
	z->avail_out = out_nbytes_avail;

	return (inflate(z, Z_FINISH) == Z_STREAM_END && z->avail_out == 0);
}

static void
libz_free_compressor(void *private)
{
	deflateEnd((z_stream *)private);
	free(private);
}

static void
libz_free_decompressor(void *private)
{
	inflateEnd((z_stream *)private);
	free(private);
}

static size_t
libdeflate_deflate_compress(void *private, const void *in, size_t in_nbytes,
			    void *out, size_t out_nbytes_avail)
{
	return deflate_compress(private, in, in_nbytes,
				out, out_nbytes_avail);
}

static bool
libdeflate_deflate_decompress(void *private, const void *in, size_t in_nbytes,
			      void *out, size_t out_nbytes_avail)
{
	return 0 == deflate_decompress(private, in, in_nbytes,
				       out, out_nbytes_avail, NULL);
}

static size_t
libdeflate_zlib_compress(void *private, const void *in, size_t in_nbytes,
			 void *out, size_t out_nbytes_avail)
{
	return zlib_compress(private, in, in_nbytes, out, out_nbytes_avail);
}

static bool
libdeflate_zlib_decompress(void *private, const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail)
{
	return 0 == zlib_decompress(private, in, in_nbytes,
				    out, out_nbytes_avail, NULL);
}

static size_t
libdeflate_gzip_compress(void *private, const void *in, size_t in_nbytes,
			 void *out, size_t out_nbytes_avail)
{
	return gzip_compress(private, in, in_nbytes, out, out_nbytes_avail);
}

static bool
libdeflate_gzip_decompress(void *private, const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail)
{
	return 0 == gzip_decompress(private, in, in_nbytes,
				    out, out_nbytes_avail, NULL);
}

static void
libdeflate_free_compressor(void *private)
{
	deflate_free_compressor(private);
}

static void
libdeflate_free_decompressor(void *private)
{
	deflate_free_decompressor(private);
}

static int
compressor_init(struct compressor *c, int level,
		enum wrapper wrapper, bool use_libz)
{
	if (use_libz) {
		z_stream *z;

		if (level > 9) {
			msg("libz only supports up to compression level 9");
			return -1;
		}

		z = xmalloc(sizeof(z_stream));
		if (z == NULL)
			return -1;

		c->private = z;

		z->next_in = NULL;
		z->avail_in = 0;
		z->zalloc = NULL;
		z->zfree = NULL;
		z->opaque = NULL;
		if (deflateInit2(z, level, Z_DEFLATED,
				 get_libz_window_bits(wrapper),
				 8, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			msg("unable to initialize deflater");
			free(z);
			return -1;
		}
		c->compress = libz_compress;
		c->free_private = libz_free_compressor;
	} else {
		c->private = alloc_compressor(level);
		if (c->private == NULL)
			return -1;
		switch (wrapper) {
		case ZLIB_WRAPPER:
			c->compress = libdeflate_zlib_compress;
			break;
		case GZIP_WRAPPER:
			c->compress = libdeflate_gzip_compress;
			break;
		default:
			c->compress = libdeflate_deflate_compress;
			break;
		}
		c->free_private = libdeflate_free_compressor;
	}
	return 0;
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

static int
decompressor_init(struct decompressor *d, enum wrapper wrapper, bool use_libz)
{
	if (use_libz) {
		z_stream *z;

		z = xmalloc(sizeof(z_stream));
		if (z == NULL)
			return -1;

		d->private = z;

		z->next_in = NULL;
		z->avail_in = 0;
		z->zalloc = NULL;
		z->zfree = NULL;
		z->opaque = NULL;
		if (inflateInit2(z, get_libz_window_bits(wrapper)) != Z_OK) {
			msg("unable to initialize inflater");
			free(z);
			return -1;
		}

		d->decompress = libz_decompress;
		d->free_private = libz_free_decompressor;
	} else {
		d->private = alloc_decompressor();
		if (d->private == NULL)
			return -1;
		switch (wrapper) {
		case ZLIB_WRAPPER:
			d->decompress = libdeflate_zlib_decompress;
			break;
		case GZIP_WRAPPER:
			d->decompress = libdeflate_gzip_decompress;
			break;
		default:
			d->decompress = libdeflate_deflate_decompress;
			break;
		}
		d->free_private = libdeflate_free_decompressor;
	}
	return 0;
}

static bool
do_decompress(struct decompressor *d, const void *in, size_t in_nbytes,
	      void *out, size_t out_nbytes_avail)
{
	return (*d->decompress)(d->private, in, in_nbytes,
				out, out_nbytes_avail);
}

static void
decompressor_destroy(struct decompressor *d)
{
	(*d->free_private)(d->private);
}

static int
do_benchmark(struct file_stream *in, void *original_buf, void *compressed_buf,
	     void *decompressed_buf, u32 chunk_size,
	     struct compressor *compressor,
	     struct decompressor *decompressor)
{
	u64 total_uncompressed_size = 0;
	u64 total_compressed_size = 0;
	u64 total_compress_time = 0;
	u64 total_decompress_time = 0;
	ssize_t ret;

	while ((ret = xread(in, original_buf, chunk_size)) > 0) {
		u32 original_size = ret;
		u32 compressed_size;
		u64 start_time;
		bool result;

		total_uncompressed_size += original_size;

		/* Compress the chunk of data. */
		start_time = current_time();
		compressed_size = do_compress(compressor,
					      original_buf,
					      original_size,
					      compressed_buf,
					      original_size - 1);
		total_compress_time += current_time() - start_time;

		if (compressed_size) {
			/* Successfully compressed the chunk of data. */

			/* Decompress the data we just compressed and compare
			 * the result with the original. */
			start_time = current_time();
			result = do_decompress(decompressor,
					       compressed_buf,
					       compressed_size,
					       decompressed_buf,
					       original_size);
			total_decompress_time += current_time() - start_time;

			if (!result) {
				msg("%"TS": failed to decompress data",
				    in->name);
				return -1;
			}

			if (memcmp(original_buf, decompressed_buf,
				   original_size) != 0)
			{
				msg("%"TS": data did not decompress to "
				    "original", in->name);
				return -1;
			}

			total_compressed_size += compressed_size;
		} else {
			/* Compression did not make the chunk smaller. */
			total_compressed_size += original_size;
		}
	}

	if (ret < 0)
		return ret;

	if (total_uncompressed_size == 0) {
		printf("\tFile was empty.\n");
		return 0;
	}

	if (total_compress_time == 0)
		total_compress_time = 1;
	if (total_decompress_time == 0)
		total_decompress_time = 1;

	printf("\tCompressed %"PRIu64 " => %"PRIu64" bytes (%u.%03u%%)\n",
	       total_uncompressed_size, total_compressed_size,
	       (unsigned int)(total_compressed_size * 100 /
				total_uncompressed_size),
	       (unsigned int)(total_compressed_size * 100000 /
				total_uncompressed_size % 1000));
	printf("\tCompression time: %"PRIu64" ms (%"PRIu64" MB/s)\n",
	       total_compress_time / 1000000,
	       1000 * total_uncompressed_size / total_compress_time);
	printf("\tDecompression time: %"PRIu64" ms (%"PRIu64" MB/s)\n",
	       total_decompress_time / 1000000,
	       1000 * total_uncompressed_size / total_decompress_time);

	return 0;
}

int
tmain(int argc, tchar *argv[])
{
	u32 chunk_size = 1048576;
	int level = 6;
	enum wrapper wrapper = NO_WRAPPER;
	bool compress_with_libz = false;
	bool decompress_with_libz = false;
	void *original_buf = NULL;
	void *compressed_buf = NULL;
	void *decompressed_buf = NULL;
	struct compressor compressor;
	struct decompressor decompressor;
	tchar *default_file_list[] = { NULL };
	int opt_char;
	int i;
	int ret;

	program_invocation_name = get_filename(argv[0]);

	while ((opt_char = tgetopt(argc, argv, optstring)) != -1) {
		switch (opt_char) {
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			level = parse_compression_level(opt_char, toptarg);
			if (level == 0)
				return -1;
			break;
		case 'g':
			wrapper = GZIP_WRAPPER;
			break;
		case 'h':
			show_usage(stdout);
			return 0;
		case 's':
			chunk_size = tstrtoul(toptarg, NULL, 10);
			if (chunk_size == 0) {
				msg("invalid chunk size: \"%"TS"\"", toptarg);
				return 1;
			}
			break;
		case 'V':
			show_version();
			return 0;
		case 'Y':
			compress_with_libz = true;
			break;
		case 'Z':
			decompress_with_libz = true;
			break;
		case 'z':
			wrapper = ZLIB_WRAPPER;
			break;
		default:
			show_usage(stderr);
			return 1;
		}
	}

	argc -= toptind;
	argv += toptind;

	original_buf = xmalloc(chunk_size);
	compressed_buf = xmalloc(chunk_size - 1);
	decompressed_buf = xmalloc(chunk_size);

	ret = -1;
	if (original_buf == NULL || compressed_buf == NULL ||
	    decompressed_buf == NULL)
		goto out0;

	ret = compressor_init(&compressor, level, wrapper, compress_with_libz);
	if (ret)
		goto out0;

	ret = decompressor_init(&decompressor, wrapper, decompress_with_libz);
	if (ret)
		goto out1;

	if (argc == 0) {
		argv = default_file_list;
		argc = ARRAY_LEN(default_file_list);
	} else {
		for (i = 0; i < argc; i++)
			if (argv[i][0] == '-' && argv[i][1] == '\0')
				argv[i] = NULL;
	}

	printf("Benchmarking DEFLATE compression:\n");
	printf("\tCompression level: %d\n", level);
	printf("\tChunk size: %"PRIu32"\n", chunk_size);
	printf("\tWrapper: %s\n",
	       wrapper == NO_WRAPPER ? "None" :
	       wrapper == ZLIB_WRAPPER ? "zlib" : "gzip");
	printf("\tCompression engine: %s\n",
	       compress_with_libz ? "libz" : "libdeflate");
	printf("\tDecompression engine: %s\n",
	       decompress_with_libz ? "libz" : "libdeflate");

	for (i = 0; i < argc; i++) {
		struct file_stream in;

		ret = xopen_for_read(argv[i], &in);
		if (ret != 0)
			goto out2;

		printf("Processing %"TS"...\n", in.name);

		ret = do_benchmark(&in, original_buf, compressed_buf,
				   decompressed_buf, chunk_size, &compressor,
				   &decompressor);
		xclose(&in);
		if (ret != 0)
			goto out2;
	}
	ret = 0;
out2:
	decompressor_destroy(&decompressor);
out1:
	compressor_destroy(&compressor);
out0:
	free(decompressed_buf);
	free(compressed_buf);
	free(original_buf);
	return -ret;
}
