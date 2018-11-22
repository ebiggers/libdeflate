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

static const tchar *const optstring = T("1::2::3::4::5::6::7::8::9::C:D:Gghs:VYZz");

enum wrapper {
	NO_WRAPPER,
	ZLIB_WRAPPER,
	GZIP_WRAPPER,
};

struct compressor {
	int level;
	enum wrapper wrapper;
	const struct engine *engine;
	void *private;
};

struct decompressor {
	enum wrapper wrapper;
	const struct engine *engine;
	void *private;
};

struct engine {
	const tchar *name;

	bool (*init_compressor)(struct compressor *);
	size_t (*compress)(struct compressor *, const void *, size_t,
			   void *, size_t);
	void (*destroy_compressor)(struct compressor *);

	bool (*init_decompressor)(struct decompressor *);
	bool (*decompress)(struct decompressor *, const void *, size_t,
			   void *, size_t);
	void (*destroy_decompressor)(struct decompressor *);
};

/******************************************************************************/

static bool
libdeflate_engine_init_compressor(struct compressor *c)
{
	c->private = alloc_compressor(c->level);
	return c->private != NULL;
}

static size_t
libdeflate_engine_compress(struct compressor *c, const void *in,
			   size_t in_nbytes, void *out, size_t out_nbytes_avail)
{
	switch (c->wrapper) {
	case ZLIB_WRAPPER:
		return libdeflate_zlib_compress(c->private, in, in_nbytes,
						out, out_nbytes_avail);
	case GZIP_WRAPPER:
		return libdeflate_gzip_compress(c->private, in, in_nbytes,
						out, out_nbytes_avail);
	default:
		return libdeflate_deflate_compress(c->private, in, in_nbytes,
						   out, out_nbytes_avail);
	}
}

static void
libdeflate_engine_destroy_compressor(struct compressor *c)
{
	libdeflate_free_compressor(c->private);
}

static bool
libdeflate_engine_init_decompressor(struct decompressor *d)
{
	d->private = alloc_decompressor();
	return d->private != NULL;
}

static bool
libdeflate_engine_decompress(struct decompressor *d, const void *in,
			     size_t in_nbytes, void *out, size_t out_nbytes)
{
	switch (d->wrapper) {
	case ZLIB_WRAPPER:
		return !libdeflate_zlib_decompress(d->private, in, in_nbytes,
						   out, out_nbytes, NULL);
	case GZIP_WRAPPER:
		return !libdeflate_gzip_decompress(d->private, in, in_nbytes,
						   out, out_nbytes, NULL);
	default:
		return !libdeflate_deflate_decompress(d->private, in, in_nbytes,
						      out, out_nbytes, NULL);
	}
}

static void
libdeflate_engine_destroy_decompressor(struct decompressor *d)
{
	libdeflate_free_decompressor(d->private);
}

static const struct engine libdeflate_engine = {
	.name			= T("libdeflate"),

	.init_compressor	= libdeflate_engine_init_compressor,
	.compress		= libdeflate_engine_compress,
	.destroy_compressor	= libdeflate_engine_destroy_compressor,

	.init_decompressor	= libdeflate_engine_init_decompressor,
	.decompress		= libdeflate_engine_decompress,
	.destroy_decompressor	= libdeflate_engine_destroy_decompressor,
};

/******************************************************************************/

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

static bool
libz_engine_init_compressor(struct compressor *c)
{
	z_stream *z;

	if (c->level > 9) {
		msg("libz only supports up to compression level 9");
		return false;
	}

	z = xmalloc(sizeof(*z));
	if (z == NULL)
		return false;

	z->next_in = NULL;
	z->avail_in = 0;
	z->zalloc = NULL;
	z->zfree = NULL;
	z->opaque = NULL;
	if (deflateInit2(z, c->level, Z_DEFLATED,
			 get_libz_window_bits(c->wrapper),
			 8, Z_DEFAULT_STRATEGY) != Z_OK)
	{
		msg("unable to initialize deflater");
		free(z);
		return false;
	}

	c->private = z;
	return true;
}

static size_t
libz_engine_compress(struct compressor *c, const void *in, size_t in_nbytes,
		     void *out, size_t out_nbytes_avail)
{
	z_stream *z = c->private;

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
libz_engine_destroy_compressor(struct compressor *c)
{
	z_stream *z = c->private;

	deflateEnd(z);
	free(z);
}

static bool
libz_engine_init_decompressor(struct decompressor *d)
{
	z_stream *z;

	z = xmalloc(sizeof(*z));
	if (z == NULL)
		return false;

	z->next_in = NULL;
	z->avail_in = 0;
	z->zalloc = NULL;
	z->zfree = NULL;
	z->opaque = NULL;
	if (inflateInit2(z, get_libz_window_bits(d->wrapper)) != Z_OK) {
		msg("unable to initialize inflater");
		free(z);
		return false;
	}

	d->private = z;
	return true;
}

static bool
libz_engine_decompress(struct decompressor *d, const void *in, size_t in_nbytes,
		       void *out, size_t out_nbytes)
{
	z_stream *z = d->private;

	inflateReset(z);

	z->next_in = (void *)in;
	z->avail_in = in_nbytes;
	z->next_out = out;
	z->avail_out = out_nbytes;

	return inflate(z, Z_FINISH) == Z_STREAM_END && z->avail_out == 0;
}

static void
libz_engine_destroy_decompressor(struct decompressor *d)
{
	z_stream *z = d->private;

	inflateEnd(z);
	free(z);
}

static const struct engine libz_engine = {
	.name			= T("libz"),

	.init_compressor	= libz_engine_init_compressor,
	.compress		= libz_engine_compress,
	.destroy_compressor	= libz_engine_destroy_compressor,

	.init_decompressor	= libz_engine_init_decompressor,
	.decompress		= libz_engine_decompress,
	.destroy_decompressor	= libz_engine_destroy_decompressor,
};

/******************************************************************************/

static const struct engine * const all_engines[] = {
	&libdeflate_engine,
	&libz_engine,
};

#define DEFAULT_ENGINE libdeflate_engine

static const struct engine *
name_to_engine(const tchar *name)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(all_engines); i++)
		if (tstrcmp(all_engines[i]->name, name) == 0)
			return all_engines[i];
	return NULL;
}

/******************************************************************************/

struct chunk_buffers {
	void *original_buf;
	void *compressed_buf;
	void *decompressed_buf;
	u8 *guarded_buf1_start, *guarded_buf1_end;
	u8 *guarded_buf2_start, *guarded_buf2_end;
};

static bool
compressor_init(struct compressor *c, int level, enum wrapper wrapper,
		const struct engine *engine)
{
	c->level = level;
	c->wrapper = wrapper;
	c->engine = engine;
	return engine->init_compressor(c);
}

static size_t
do_compress(struct compressor *c, struct chunk_buffers *buffers,
	    size_t original_size, size_t max_compressed_size)
{
	void *in = buffers->original_buf;
	void *out = buffers->compressed_buf;
	size_t compressed_size;

	if (buffers->guarded_buf1_start != NULL) {
		in = buffers->guarded_buf1_end - original_size;
		out = buffers->guarded_buf2_end - max_compressed_size;
		memcpy(in, buffers->original_buf, original_size);
	}

	compressed_size = c->engine->compress(c, in, original_size,
					      out, max_compressed_size);

	if (out != buffers->compressed_buf)
		memcpy(buffers->compressed_buf, out, compressed_size);

	return compressed_size;
}

static void
compressor_destroy(struct compressor *c)
{
	c->engine->destroy_compressor(c);
}

static bool
decompressor_init(struct decompressor *d, enum wrapper wrapper,
		  const struct engine *engine)
{
	d->wrapper = wrapper;
	d->engine = engine;
	return engine->init_decompressor(d);
}

static bool
do_decompress(struct decompressor *d, struct chunk_buffers *buffers,
	      size_t compressed_size, size_t original_size)
{
	void *in = buffers->compressed_buf;
	void *out = buffers->decompressed_buf;
	bool ok;

	if (buffers->guarded_buf1_start != NULL) {
		in = buffers->guarded_buf1_end - compressed_size;
		out = buffers->guarded_buf2_end - original_size;
		memcpy(in, buffers->compressed_buf, compressed_size);
	}

	ok = d->engine->decompress(d, in, compressed_size, out, original_size);

	if (ok && out != buffers->decompressed_buf)
		memcpy(buffers->decompressed_buf, out, original_size);

	return ok;
}

static void
decompressor_destroy(struct decompressor *d)
{
	d->engine->destroy_decompressor(d);
}

/******************************************************************************/

static void
show_available_engines(FILE *fp)
{
	size_t i;

	fprintf(fp, "Available ENGINEs are: ");
	for (i = 0; i < ARRAY_LEN(all_engines); i++) {
		fprintf(fp, "%"TS, all_engines[i]->name);
		if (i < ARRAY_LEN(all_engines) - 1)
			fprintf(fp, ", ");
	}
	fprintf(fp, ".  Default is %"TS"\n", DEFAULT_ENGINE.name);
}

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %"TS" [-LVL] [-C ENGINE] [-D ENGINE] [-ghVz] [-s SIZE] [FILE]...\n"
"Benchmark DEFLATE compression and decompression on the specified FILEs.\n"
"\n"
"Options:\n"
"  -1        fastest (worst) compression\n"
"  -6        medium compression (default)\n"
"  -12       slowest (best) compression\n"
"  -C ENGINE compression engine\n"
"  -D ENGINE decompression engine\n"
"  -G        test with guard pages\n"
"  -g        use gzip wrapper\n"
"  -h        print this help\n"
"  -s SIZE   chunk size\n"
"  -V        show version and legal information\n"
"  -z        use zlib wrapper\n"
"\n", program_invocation_name);

	show_available_engines(fp);
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


/******************************************************************************/

static void free_chunk_buffers(struct chunk_buffers *buffers)
{
	free(buffers->original_buf);
	free(buffers->compressed_buf);
	free(buffers->decompressed_buf);
	free_guarded_buffer(buffers->guarded_buf1_start,
			    buffers->guarded_buf1_end);
	free_guarded_buffer(buffers->guarded_buf2_start,
			    buffers->guarded_buf2_end);
	memset(buffers, 0, sizeof(*buffers));
}

static bool alloc_chunk_buffers(struct chunk_buffers *buffers,
				u32 chunk_size, bool use_guard_pages)
{
	int res = 0;

	memset(buffers, 0, sizeof(*buffers));

	buffers->original_buf = xmalloc(chunk_size);
	buffers->compressed_buf = xmalloc(chunk_size - 1);
	buffers->decompressed_buf = xmalloc(chunk_size);

	if (use_guard_pages) {
		res |= alloc_guarded_buffer(chunk_size,
					    &buffers->guarded_buf1_start,
					    &buffers->guarded_buf1_end);
		res |= alloc_guarded_buffer(chunk_size,
					    &buffers->guarded_buf2_start,
					    &buffers->guarded_buf2_end);
	}
	if (buffers->original_buf == NULL || buffers->compressed_buf == NULL ||
	    buffers->decompressed_buf == NULL || res != 0) {
		free_chunk_buffers(buffers);
		return false;
	}
	return true;
}

static int
do_benchmark(struct file_stream *in, struct chunk_buffers *buffers,
	     u32 chunk_size, struct compressor *compressor,
	     struct decompressor *decompressor)
{
	u64 total_uncompressed_size = 0;
	u64 total_compressed_size = 0;
	u64 total_compress_time = 0;
	u64 total_decompress_time = 0;
	ssize_t ret;

	while ((ret = xread(in, buffers->original_buf, chunk_size)) > 0) {
		u32 original_size = ret;
		u32 compressed_size;
		u64 start_time;
		bool ok;

		total_uncompressed_size += original_size;

		/* Compress the chunk of data. */
		start_time = timer_ticks();
		compressed_size = do_compress(compressor, buffers,
					      original_size, original_size - 1);
		total_compress_time += timer_ticks() - start_time;

		if (compressed_size) {
			/* Successfully compressed the chunk of data. */

			/* Decompress the data we just compressed and compare
			 * the result with the original. */
			start_time = timer_ticks();
			ok = do_decompress(decompressor, buffers,
					   compressed_size, original_size);
			total_decompress_time += timer_ticks() - start_time;

			if (!ok) {
				msg("%"TS": failed to decompress data",
				    in->name);
				return -1;
			}

			if (memcmp(buffers->original_buf,
				   buffers->decompressed_buf,
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
	       timer_ticks_to_ms(total_compress_time),
	       timer_MB_per_s(total_uncompressed_size, total_compress_time));
	printf("\tDecompression time: %"PRIu64" ms (%"PRIu64" MB/s)\n",
	       timer_ticks_to_ms(total_decompress_time),
	       timer_MB_per_s(total_uncompressed_size, total_decompress_time));

	return 0;
}

int
tmain(int argc, tchar *argv[])
{
	u32 chunk_size = 1048576;
	int level = 6;
	enum wrapper wrapper = NO_WRAPPER;
	const struct engine *compress_engine = &DEFAULT_ENGINE;
	const struct engine *decompress_engine = &DEFAULT_ENGINE;
	struct chunk_buffers buffers;
	struct compressor compressor;
	struct decompressor decompressor;
	tchar *default_file_list[] = { NULL };
	bool use_guard_pages = false;
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
				return 1;
			break;
		case 'C':
			compress_engine = name_to_engine(toptarg);
			if (compress_engine == NULL) {
				msg("invalid compression engine: \"%"TS"\"", toptarg);
				show_available_engines(stderr);
				return 1;
			}
			break;
		case 'D':
			decompress_engine = name_to_engine(toptarg);
			if (decompress_engine == NULL) {
				msg("invalid decompression engine: \"%"TS"\"", toptarg);
				show_available_engines(stderr);
				return 1;
			}
			break;
		case 'G':
			use_guard_pages = true;
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
		case 'Y': /* deprecated, use '-C libz' instead */
			compress_engine = &libz_engine;
			break;
		case 'Z': /* deprecated, use '-D libz' instead */
			decompress_engine = &libz_engine;
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

	ret = -1;

	if (!alloc_chunk_buffers(&buffers, chunk_size, use_guard_pages))
		goto out0;

	if (!compressor_init(&compressor, level, wrapper, compress_engine))
		goto out0;

	if (!decompressor_init(&decompressor, wrapper, decompress_engine))
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
	printf("\tCompression engine: %"TS"\n", compress_engine->name);
	printf("\tDecompression engine: %"TS"\n", decompress_engine->name);
	if (use_guard_pages)
		printf("\tDebugging options: guard_pages\n");

	for (i = 0; i < argc; i++) {
		struct file_stream in;

		ret = xopen_for_read(argv[i], true, &in);
		if (ret != 0)
			goto out2;

		printf("Processing %"TS"...\n", in.name);

		ret = do_benchmark(&in, &buffers, chunk_size, &compressor,
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
	free_chunk_buffers(&buffers);
	return -ret;
}
