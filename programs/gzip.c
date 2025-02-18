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
#include "gzip_compress_by_stream.h"
#include "gzip_decompress_by_stream.h"

#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <sys/utime.h>
#else
#  include <sys/time.h>
#  include <unistd.h>
#  include <utime.h>
#endif

struct options {
	bool to_stdout;
	bool decompress;
	bool force;
	bool keep;
	bool test;
	int compression_level;
	const tchar *suffix;
};

static const tchar *const optstring = T("1::2::3::4::5::6::7::8::9::cdfhknqS:tV");

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %"TS" [-LEVEL] [-cdfhkqtV] [-S SUF] FILE...\n"
"Compress or decompress the specified FILEs.\n"
"\n"
"Options:\n"
"  -1        fastest (worst) compression\n"
"  -6        medium compression (default)\n"
"  -12       slowest (best) compression\n"
"  -c        write to standard output\n"
"  -d        decompress\n"
"  -f        overwrite existing output files; (de)compress hard-linked files;\n"
"            allow reading/writing compressed data from/to terminal;\n"
"            with gunzip -c, pass through non-gzipped data\n"
"  -h        print this help\n"
"  -k        don't delete input files\n"
"  -q        suppress warnings\n"
"  -S SUF    use suffix SUF instead of .gz\n"
"  -t        test file integrity\n"
"  -V        show version and legal information\n",
	prog_invocation_name);
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
	if (tstrxcmp(prog_invocation_name, T("gunzip")) == 0)
		return true;
	if (tstrxcmp(prog_invocation_name, T("libdeflate-gunzip")) == 0)
		return true;
#ifdef _WIN32
	if (tstrxcmp(prog_invocation_name, T("gunzip.exe")) == 0)
		return true;
	if (tstrxcmp(prog_invocation_name, T("libdeflate-gunzip.exe")) == 0)
		return true;
#endif
	return false;
}

static const tchar *
get_suffix(const tchar *path, const tchar *suffix)
{
	size_t path_len = tstrlen(path);
	size_t suffix_len = tstrlen(suffix);
	const tchar *p;

	if (path_len <= suffix_len)
		return NULL;
	p = &path[path_len - suffix_len];
	if (tstrxcmp(p, suffix) == 0)
		return p;
	return NULL;
}

static bool
has_suffix(const tchar *path, const tchar *suffix)
{
	return get_suffix(path, suffix) != NULL;
}

static tchar *
append_suffix(const tchar *path, const tchar *suffix)
{
	size_t path_len = tstrlen(path);
	size_t suffix_len = tstrlen(suffix);
	tchar *suffixed_path;

	suffixed_path = xmalloc((path_len + suffix_len + 1) * sizeof(tchar));
	if (suffixed_path == NULL)
		return NULL;
	tmemcpy(suffixed_path, path, path_len);
	tmemcpy(&suffixed_path[path_len], suffix, suffix_len + 1);
	return suffixed_path;
}

static int
stat_file(struct file_stream *in, stat_t *stbuf, bool allow_hard_links)
{
	if (tfstat(in->fd, stbuf) != 0) {
		msg("%"TS": unable to stat file", in->name);
		return -1;
	}

	if (!S_ISREG(stbuf->st_mode) && !in->is_standard_stream) {
		warn("%"TS" is %s -- skipping",
		     in->name, S_ISDIR(stbuf->st_mode) ? "a directory" :
							 "not a regular file");
		return -2;
	}

	if (stbuf->st_nlink > 1 && !allow_hard_links) {
		warn("%"TS" has multiple hard links -- skipping (use -f to process anyway)",
		     in->name);
		return -2;
	}

	return 0;
}

static void
restore_mode(struct file_stream *out, const stat_t *stbuf)
{
#ifndef _WIN32
	if (fchmod(out->fd, stbuf->st_mode) != 0)
		msg_errno("%"TS": unable to preserve mode", out->name);
#endif
}

static void
restore_owner_and_group(struct file_stream *out, const stat_t *stbuf)
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
		   const stat_t *stbuf)
{
	int ret;
#ifdef __APPLE__
	struct timespec times[2] = { stbuf->st_atimespec, stbuf->st_mtimespec };

	ret = futimens(out->fd, times);
#elif (defined(HAVE_FUTIMENS) && defined(HAVE_STAT_NANOSECOND_PRECISION)) || \
	/* fallback detection method for direct compilation */ \
	(!defined(HAVE_CONFIG_H) && defined(UTIME_NOW))
	struct timespec times[2] = { stbuf->st_atim, stbuf->st_mtim };

	ret = futimens(out->fd, times);
#else
	struct tutimbuf times = { stbuf->st_atime, stbuf->st_mtime };

	ret = tutime(newpath, &times);
#endif
	if (ret != 0)
		msg_errno("%"TS": unable to preserve timestamps", out->name);
}

static void
restore_metadata(struct file_stream *out, const tchar *newpath,
		 const stat_t *stbuf)
{
	restore_mode(out, stbuf);
	restore_owner_and_group(out, stbuf);
	restore_timestamps(out, newpath, stbuf);
}

static int
decompress_file(struct libdeflate_decompressor *decompressor, const tchar *path,
		const struct options *options)
{
	tchar *oldpath = (tchar *)path;
	tchar *newpath = NULL;
	struct file_stream in;
	struct file_stream out;
	stat_t stbuf;
	int ret;
	int ret2;

	if (path != NULL) {
		const tchar *suffix = get_suffix(path, options->suffix);
		if (suffix == NULL) {
			/*
			 * Input file is unsuffixed.  If the file doesn't exist,
			 * then try it suffixed.  Otherwise, if we're not
			 * writing to stdout, skip the file with warning status.
			 * Otherwise, go ahead and try to open the file anyway
			 * (which will very likely fail).
			 */
			if (tstat(path, &stbuf) != 0 && errno == ENOENT) {
				oldpath = append_suffix(path, options->suffix);
				if (oldpath == NULL)
					return -1;
				if (!options->to_stdout)
					newpath = (tchar *)path;
			} else if (!options->to_stdout) {
				warn("\"%"TS"\" does not end with the %"TS" suffix -- skipping",
				     path, options->suffix);
				return -2;
			}
		} else if (!options->to_stdout) {
			/*
			 * Input file is suffixed, and we're not writing to
			 * stdout.  Strip the suffix to get the path to the
			 * output file.
			 */
			newpath = xmalloc((suffix - oldpath + 1) *
					  sizeof(tchar));
			if (newpath == NULL)
				return -1;
			tmemcpy(newpath, oldpath, suffix - oldpath);
			newpath[suffix - oldpath] = '\0';
		}
	}

	ret = xopen_for_read(oldpath, options->force || options->to_stdout,
			     &in);
	if (ret != 0)
		goto out_free_paths;

	if (!options->force && isatty(in.fd)) {
		msg("Refusing to read compressed data from terminal.  "
		    "Use -f to override.\nFor help, use -h.");
		ret = -1;
		goto out_close_in;
	}

	ret = stat_file(&in, &stbuf, options->force || options->keep ||
			oldpath == NULL || newpath == NULL);
	if (ret != 0)
		goto out_close_in;

	ret = xopen_for_write(newpath, options->force, &out);
	if (ret != 0)
		goto out_close_in;

    ret = gzips_decompress_by_stream(decompressor, &in, stbuf.st_size, (options->test?0:&out), 
									NULL, NULL);
	if (ret != 0){
		msg("\nERROR: gzips_decompress_by_stream() error code %d\n\n",ret);
		goto out_close_out;
	}

	if (oldpath != NULL && newpath != NULL)
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
	if (ret == 0 && oldpath != NULL && newpath != NULL && !options->keep)
		tunlink(oldpath);
out_free_paths:
	if (newpath != path)
		free(newpath);
	if (oldpath != path)
		free(oldpath);
	return ret;
}

static int
compress_file(int compression_level, const tchar *path,
	      const struct options *options)
{
	tchar *newpath = NULL;
	struct file_stream in;
	struct file_stream out;
	stat_t stbuf;
	int ret;
	int ret2;

	if (path != NULL && !options->to_stdout) {
		if (!options->force && has_suffix(path, options->suffix)) {
			msg("%"TS": already has %"TS" suffix -- skipping",
			    path, options->suffix);
			return 0;
		}
		newpath = append_suffix(path, options->suffix);
		if (newpath == NULL)
			return -1;
	}

	ret = xopen_for_read(path, options->force || options->to_stdout, &in);
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

	ret = gzip_compress_by_stream(compression_level, &in, stbuf.st_size,&out, NULL);
	if (ret != 0){
		msg("\nERROR: gzip_compress_by_stream() error code %d\n\n",ret);
		goto out_close_out;
	}

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
	return ret;
}

int
tmain(int argc, tchar *argv[])
{
	tchar *default_file_list[] = { NULL };
	struct options options;
	int opt_char;
	int i;
	int ret;

	begin_program(argv);

	options.to_stdout = false;
	options.decompress = is_gunzip();
	options.force = false;
	options.keep = false;
	options.test = false;
	options.compression_level = 6;
	options.suffix = T(".gz");

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
			if (options.compression_level < 0)
				return 1;
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
		case 'n':
			/*
			 * -n means don't save or restore the original filename
			 *  in the gzip header.  Currently this implementation
			 *  already behaves this way by default, so accept the
			 *  option as a no-op.
			 */
			break;
		case 'q':
			suppress_warnings = true;
			break;
		case 'S':
			options.suffix = toptarg;
			if (options.suffix[0] == T('\0')) {
				msg("invalid suffix");
				return 1;
			}
			break;
		case 't':
			options.test = true;
			options.decompress = true;
			options.to_stdout = true;
			/*
			 * -t behaves just like the more commonly used -c
			 * option, except that -t doesn't actually write
			 * anything.  For ease of implementation, just pretend
			 * that -c was specified too.
			 */
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
		argv = default_file_list;
		argc = ARRAY_LEN(default_file_list);
	} else {
		for (i = 0; i < argc; i++)
			if (argv[i][0] == '-' && argv[i][1] == '\0')
				argv[i] = NULL;
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
		for (i = 0; i < argc; i++)
			ret |= -compress_file(options.compression_level, argv[i], &options);
	}

	switch (ret) {
	case 0:
		/* No warnings or errors */
		return 0;
	case 2:
		/* At least one warning, but no errors */
		if (suppress_warnings)
			return 0;
		return 2;
	default:
		/* At least one error */
		return 1;
	}
}
