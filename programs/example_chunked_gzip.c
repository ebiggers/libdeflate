/*
 * example_chunked_gzip.c - Example demonstrating chunked streaming gzip compression
 *                          using libdeflate with constant bounded memory.
 *
 * Copyright 2026 Eric Biggers and contributors
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
 *
 * ---------------------------------------------------------------------------
 *
 * Approach:
 *   libdeflate is designed for whole-buffer in-memory compression. For streaming
 *   large files (or standard input/output pipelines), each chunk can be compressed
 *   into a standard gzip member. Under RFC 1952 (section 2.2), any number of
 *   gzip members can be concatenated to form a single valid gzip stream.
 *   Standard utilities (gzip, pigz, gunzip, tar, libarchive) decompress the
 *   entire concatenated stream transparently.
 */

#include "prog_util.h"
#include "libdeflate.h"

#define CHUNK_SIZE (1024 * 1024) /* 1 MB per chunk */

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %"TS" [LEVEL] < INPUT > OUTPUT.gz\n"
"Demonstrate chunked streaming compression into a valid gzip stream.\n"
"\n"
"Arguments:\n"
"  LEVEL     compression level (1-12, default 6)\n",
	prog_invocation_name);
}

int
tmain(int argc, tchar *argv[])
{
	int compression_level = 6;
	struct libdeflate_compressor *compressor = NULL;
	void *in_buf = NULL;
	void *out_buf = NULL;
	size_t max_out_size = 0;
	FILE *in_fp = stdin;
	FILE *out_fp = stdout;
	size_t bytes_read = 0;
	size_t total_in = 0;
	size_t total_out = 0;
	int ret = 1;

	begin_program(argv);

	if (argc > 1) {
		if (tstrcmp(argv[1], T("-h")) == 0 ||
		    tstrcmp(argv[1], T("--help")) == 0) {
			show_usage(stdout);
			return 0;
		}
		compression_level = (int)tstrtoul(argv[1], NULL, 10);
		if (compression_level < 1 || compression_level > 12) {
			msg("invalid compression level (must be 1-12)");
			show_usage(stderr);
			return 1;
		}
	}

	compressor = alloc_compressor(compression_level);
	if (compressor == NULL)
		goto cleanup;

	in_buf = xmalloc(CHUNK_SIZE);
	max_out_size = libdeflate_gzip_compress_bound(compressor, CHUNK_SIZE);
	out_buf = xmalloc(max_out_size);

	/* Read and compress chunk by chunk */
	while ((bytes_read = fread(in_buf, 1, CHUNK_SIZE, in_fp)) > 0) {
		size_t comp_size;

		total_in += bytes_read;
		comp_size = libdeflate_gzip_compress(
			compressor, in_buf, bytes_read, out_buf, max_out_size);

		if (comp_size == 0) {
			msg("libdeflate_gzip_compress failed");
			goto cleanup;
		}

		if (fwrite(out_buf, 1, comp_size, out_fp) != comp_size) {
			msg_errno("failed to write compressed output");
			goto cleanup;
		}
		total_out += comp_size;
	}

	if (ferror(in_fp)) {
		msg_errno("error reading input stream");
		goto cleanup;
	}

	if (!suppress_warnings) {
		fprintf(stderr,
			"Streamed %zu uncompressed bytes -> %zu compressed bytes (ratio %.2f%%)\n",
			total_in, total_out,
			total_in > 0 ? (double)total_out / (double)total_in * 100.0 : 0.0);
	}
	ret = 0;

cleanup:
	if (compressor != NULL)
		libdeflate_free_compressor(compressor);
	free(in_buf);
	free(out_buf);
	return ret;
}
