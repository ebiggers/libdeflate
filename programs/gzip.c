/*
 * gzip.c - a file compression and decompression program
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

#include "prog_util.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <sys/utime.h>
#else
#  include <sys/time.h>
#  include <unistd.h>
#  include <utime.h>
#endif


#ifdef __APPLE__
#ifndef st_mtim
#define st_mtim st_mtimespec
#endif
#ifndef st_atim
#define st_atim st_atimespec
#endif
#endif

struct options {
	bool to_stdout;
	bool decompress;
	bool force;
	bool keep;
	int compression_level;
	const tchar *suffix;
};

static const tchar *const optstring = T("1::2::3::4::5::6::7::8::9::cdfhkS:V");

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %"TS" [-LEVEL] [-cdfhkV] [-S SUF] FILE...\n"
"Compress or decompress the specified FILEs.\n"
"\n"
"Options:\n"
"  -1        fastest (worst) compression\n"
"  -6        medium compression (default)\n"
"  -12       slowest (best) compression\n"
"  -c        write to standard output\n"
"  -d        decompress\n"
"  -f        overwrite existing output files\n"
"  -h        print this help\n"
"  -k        don't delete input files\n"
"  -S SUF    use suffix .SUF instead of .gz\n"
"  -V        show version and legal information\n",
	program_invocation_name);
}

static void
show_version(void)
{
	printf(
"gzip compression program v" LIBDEFLATE_VERSION_STRING "\n"
"Copyright 2016 Eric Biggers\n"
"\n"
"This program is free software which may be modified and/or redistributed\n"
"under the terms of the MIT license.  There is NO WARRANTY, to the extent\n"
"permitted by law.  See the COPYING file for details.\n"
	);
}

/* Was the program invoked in decompression mode? */
static bool
is_gunzip(void)
{
	if (tstrxcmp(program_invocation_name, T("gunzip")) == 0)
		return true;
#ifdef _WIN32
	if (tstrxcmp(program_invocation_name, T("gunzip.exe")) == 0)
		return true;
#endif
	return false;
}

static const tchar *
get_suffix(const tchar *path, const tchar *suffix)
{
	const tchar *dot = tstrrchr(get_filename(path), '.');

	if (dot != NULL && tstrxcmp(dot + 1, suffix) == 0)
		return dot;
	return NULL;
}

static bool
has_suffix(const tchar *path, const tchar *suffix)
{
	return get_suffix(path, suffix) != NULL;
}

static int
do_compress(struct libdeflate_compressor *compressor,
	    struct file_stream *in, struct file_stream *out)
{
	const void *uncompressed_data = in->mmap_mem;
	size_t uncompressed_size = in->mmap_size;
	void *compressed_data;
	size_t actual_compressed_size;
	size_t max_compressed_size;
	int ret;

	max_compressed_size = libdeflate_gzip_compress_bound(compressor,
							     uncompressed_size);
	compressed_data = xmalloc(max_compressed_size);
	if (compressed_data == NULL) {
		msg("%"TS": file is probably too large to be processed by this "
		    "program", in->name);
		ret = -1;
		goto out;
	}

	actual_compressed_size = libdeflate_gzip_compress(compressor,
							  uncompressed_data,
							  uncompressed_size,
							  compressed_data,
							  max_compressed_size);
	if (actual_compressed_size == 0) {
		msg("Bug in libdeflate_gzip_compress_bound()!");
		ret = -1;
		goto out;
	}

	ret = full_write(out, compressed_data, actual_compressed_size);
out:
	free(compressed_data);
	return ret;
}

static u32
load_u32_gzip(const u8 *p)
{
	return ((u32)p[0] << 0) | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int
do_decompress(struct libdeflate_decompressor *decompressor,
	      struct file_stream *in, struct file_stream *out)
{
	const u8 *compressed_data = in->mmap_mem;
	size_t compressed_size = in->mmap_size;
	void *uncompressed_data = NULL;
	size_t uncompressed_size;
	enum libdeflate_result result;
	int ret;

	if (compressed_size < sizeof(u32)) {
	       msg("%"TS": not in gzip format", in->name);
	       ret = -1;
	       goto out;
	}

	uncompressed_size = load_u32_gzip(&compressed_data[compressed_size - 4]);

	uncompressed_data = xmalloc(uncompressed_size);
	if (uncompressed_data == NULL) {
		msg("%"TS": file is probably too large to be processed by this "
		    "program", in->name);
		ret = -1;
		goto out;
	}

	result = libdeflate_gzip_decompress(decompressor,
					    compressed_data,
					    compressed_size,
					    uncompressed_data,
					    uncompressed_size, NULL);

	if (result == LIBDEFLATE_INSUFFICIENT_SPACE) {
		msg("%"TS": file corrupt or too large to be processed by this "
		    "program", in->name);
		ret = -1;
		goto out;
	}

	if (result != LIBDEFLATE_SUCCESS) {
		msg("%"TS": file corrupt or not in gzip format", in->name);
		ret = -1;
		goto out;
	}

	ret = full_write(out, uncompressed_data, uncompressed_size);
out:
	free(uncompressed_data);
	return ret;
}

static int
stat_file(struct file_stream *in, struct stat *stbuf, bool allow_hard_links)
{
	if (fstat(in->fd, stbuf) != 0) {
		msg("%"TS": unable to stat file", in->name);
		return -1;
	}

	if (!S_ISREG(stbuf->st_mode) && !in->is_standard_stream) {
		msg("%"TS" is %s -- skipping",
		    in->name, S_ISDIR(stbuf->st_mode) ? "a directory" :
							"not a regular file");
		return -2;
	}

	if (stbuf->st_nlink > 1 && !allow_hard_links) {
		msg("%"TS" has multiple hard links -- skipping "
		    "(use -f to process anyway)", in->name);
		return -2;
	}

	return 0;
}

static void
restore_mode(struct file_stream *out, const struct stat *stbuf)
{
#ifndef _WIN32
	if (fchmod(out->fd, stbuf->st_mode) != 0)
		msg_errno("%"TS": unable to preserve mode", out->name);
#endif
}

static void
restore_owner_and_group(struct file_stream *out, const struct stat *stbuf)
{
#ifndef _WIN32
	if (fchown(out->fd, stbuf->st_uid, stbuf->st_gid) != 0) {
		msg_errno("%"TS": unable to preserve owner and group",
			  out->name);
	}
#endif
}

static void
restore_timestamps(struct file_stream *out, const tchar *newpath,
		   const struct stat *stbuf)
{
	int ret;
#if defined(HAVE_FUTIMENS)
	struct timespec times[2] = {
		stbuf->st_atim, stbuf->st_mtim,
	};
	ret = futimens(out->fd, times);
#elif defined(HAVE_FUTIMES)
	struct timeval times[2] = {
		{ stbuf->st_atim.tv_sec, stbuf->st_atim.tv_nsec / 1000, },
		{ stbuf->st_mtim.tv_sec, stbuf->st_mtim.tv_nsec / 1000, },
	};
	ret = futimes(out->fd, times);
#else /* HAVE_FUTIMES */
	struct tutimbuf times = {
		stbuf->st_atime, stbuf->st_mtime,
	};
	ret = tutime(newpath, &times);
#endif /* !HAVE_FUTIMES */
	if (ret != 0)
		msg_errno("%"TS": unable to preserve timestamps", out->name);
}

static void
restore_metadata(struct file_stream *out, const tchar *newpath,
		 const struct stat *stbuf)
{
	restore_mode(out, stbuf);
	restore_owner_and_group(out, stbuf);
	restore_timestamps(out, newpath, stbuf);
}

static int
decompress_file(struct libdeflate_decompressor *decompressor, const tchar *path,
		const struct options *options)
{
	tchar *newpath = NULL;
	struct file_stream in;
	struct file_stream out;
	struct stat stbuf;
	int ret;
	int ret2;

	if (path != NULL && !options->to_stdout) {
		const tchar *suffix = get_suffix(path, options->suffix);
		if (suffix == NULL) {
			msg("\"%"TS"\" does not end with the .%"TS" suffix -- "
			    "skipping", path, options->suffix);
			ret = -2;
			goto out;
		}
		newpath = xmalloc((suffix - path + 1) * sizeof(tchar));
		tmemcpy(newpath, path, suffix - path);
		newpath[suffix - path] = '\0';
	}

	ret = xopen_for_read(path, &in);
	if (ret != 0)
		goto out_free_newpath;

	if (!options->force && isatty(in.fd)) {
		msg("Refusing to read compressed data from terminal.  "
		    "Use -f to override.\nFor help, use -h.");
		ret = -1;
		goto out_close_in;
	}

	ret = stat_file(&in, &stbuf, options->force || options->keep ||
			path == NULL || newpath == NULL);
	if (ret != 0)
		goto out_close_in;

	ret = xopen_for_write(newpath, options->force, &out);
	if (ret != 0)
		goto out_close_in;

	ret = map_file_contents(&in, stbuf.st_size);
	if (ret != 0)
		goto out_close_out;

	ret = do_decompress(decompressor, &in, &out);
	if (ret != 0)
		goto out_close_out;

	if (path != NULL && newpath != NULL)
		restore_metadata(&out, newpath, &stbuf);
	ret = 0;
out_close_out:
	ret2 = xclose(&out);
	if (ret == 0)
		ret = ret2;
	if (ret != 0 && newpath != NULL)
		tunlink(newpath);
out_close_in:
	xclose(&in);
	if (ret == 0 && path != NULL && newpath != NULL && !options->keep)
		tunlink(path);
out_free_newpath:
	free(newpath);
out:
	return ret;
}

static int
compress_file(struct libdeflate_compressor *compressor, const tchar *path,
	      const struct options *options)
{
	tchar *newpath = NULL;
	struct file_stream in;
	struct file_stream out;
	struct stat stbuf;
	int ret;
	int ret2;

	if (path != NULL && !options->to_stdout) {
		size_t path_nchars, suffix_nchars;

		if (!options->force && has_suffix(path, options->suffix)) {
			msg("%"TS": already has .%"TS" suffix -- skipping",
			    path, options->suffix);
			ret = -2;
			goto out;
		}
		path_nchars = tstrlen(path);
		suffix_nchars = tstrlen(options->suffix);
		newpath = xmalloc((path_nchars + 1 + suffix_nchars + 1) *
					sizeof(tchar));
		tmemcpy(newpath, path, path_nchars);
		newpath[path_nchars] = '.';
		tmemcpy(&newpath[path_nchars + 1], options->suffix,
			suffix_nchars + 1);
	}

	ret = xopen_for_read(path, &in);
	if (ret != 0)
		goto out_free_newpath;

	ret = stat_file(&in, &stbuf, options->force || options->keep ||
			path == NULL || newpath == NULL);
	if (ret != 0)
		goto out_close_in;

	ret = xopen_for_write(newpath, options->force, &out);
	if (ret != 0)
		goto out_close_in;

	if (!options->force && isatty(out.fd)) {
		msg("Refusing to write compressed data to terminal. "
		    "Use -f to override.\nFor help, use -h.");
		ret = -1;
		goto out_close_out;
	}

	ret = map_file_contents(&in, stbuf.st_size);
	if (ret)
		goto out_close_out;

	ret = do_compress(compressor, &in, &out);
	if (ret != 0)
		goto out_close_out;

	if (path != NULL && newpath != NULL)
		restore_metadata(&out, newpath, &stbuf);
	ret = 0;
out_close_out:
	ret2 = xclose(&out);
	if (ret == 0)
		ret = ret2;
	if (ret != 0 && newpath != NULL)
		tunlink(newpath);
out_close_in:
	xclose(&in);
	if (ret == 0 && path != NULL && newpath != NULL && !options->keep)
		tunlink(path);
out_free_newpath:
	free(newpath);
out:
	return ret;
}

int
tmain(int argc, tchar *argv[])
{
	struct options options;
	int opt_char;
	int i;
	int ret;

	program_invocation_name = get_filename(argv[0]);

	options.to_stdout = false;
	options.decompress = is_gunzip();
	options.force = false;
	options.keep = false;
	options.compression_level = 6;
	options.suffix = T("gz");

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
			options.compression_level =
				parse_compression_level(opt_char, toptarg);
			if (options.compression_level == 0)
				return -1;
			break;
		case 'c':
			options.to_stdout = true;
			break;
		case 'd':
			options.decompress = true;
			break;
		case 'f':
			options.force = true;
			break;
		case 'h':
			show_usage(stdout);
			return 0;
		case 'k':
			options.keep = true;
			break;
		case 'S':
			options.suffix = toptarg;
			break;
		case 'V':
			show_version();
			return 0;
		default:
			show_usage(stderr);
			return 1;
		}
	}

	argv += toptind;
	argc -= toptind;

	if (argc == 0) {
		show_usage(stderr);
		return 1;
	}
	for (i = 0; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == '\0') {
			msg("This implementation of gzip does not yet "
			    "support reading from standard input.");
			return 1;
		}
	}

	ret = 0;
	if (options.decompress) {
		struct libdeflate_decompressor *d;

		d = alloc_decompressor();
		if (d == NULL)
			return 1;

		for (i = 0; i < argc; i++)
			ret |= -decompress_file(d, argv[i], &options);

		libdeflate_free_decompressor(d);
	} else {
		struct libdeflate_compressor *c;

		c = alloc_compressor(options.compression_level);
		if (c == NULL)
			return 1;

		for (i = 0; i < argc; i++)
			ret |= -compress_file(c, argv[i], &options);

		libdeflate_free_compressor(c);
	}

	/*
	 * If ret=0, there were no warnings or errors.  Exit with status 0.
	 * If ret=2, there was at least one warning.  Exit with status 2.
	 * Else, there was at least one error.  Exit with status 1.
	 */
	if (ret != 0 && ret != 2)
		ret = 1;

	return ret;
}
