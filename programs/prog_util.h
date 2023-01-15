/*
 * prog_util.h - common header for the programs; must be included first
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

#ifndef PROGRAMS_PROG_UTIL_H
#define PROGRAMS_PROG_UTIL_H

/*
 * This header provides some utility functions and macros for the programs.  It
 * also defines some macros that control the behavior of system headers, and for
 * that reason it must be included before any system header.
 *
 * The latter part could be handled in this directory's CMakeLists.txt instead.
 * We put as much as possible here, directly in the source, to make it easier to
 * build the programs using other build systems (or "no build system").
 *
 * Note: CMakeLists.txt does do some dynamic feature detection, which can't be
 * done in the source code.  For that reason, it duplicates some of the logic
 * that defines macros like _GNU_SOURCE.  Keep this logic in sync.
 */

#ifdef _WIN32

  /*
   * To keep the code similar on all platforms, sometimes we intentionally use
   * the "deprecated" non-underscore-prefixed variants of functions in msvcrt.
   */
#  undef _CRT_NONSTDC_NO_DEPRECATE
#  define _CRT_NONSTDC_NO_DEPRECATE	1

  /*
   * Similarly, to match other platforms we intentionally use the "non-secure"
   * variants, which aren't actually any less secure when used properly.
   */
#  undef _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_WARNINGS	1

#else

   /* Needed to work with files >= 2 GiB on 32-bit systems */
#  undef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS	64

   /* Note: when making changes here, update programs/CMakeLists.txt too. */
#  if defined(__linux__)
     /*
      * May be needed for clock_gettime(), posix_fadvise(), posix_madvise(),
      * futimens(), and MAP_ANONYMOUS, depending on the C library version.
      */
#    undef _GNU_SOURCE
#    define _GNU_SOURCE
#    undef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE	200809L
#  elif defined(__APPLE__)
     /* Needed for O_NOFOLLOW and MAP_ANON */
#    undef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#    undef _POSIX_C_SOURCE
#  elif defined(__sun)
     /* Needed for futimens() */
#    undef __EXTENSIONS__
#    define __EXTENSIONS__
#    undef _POSIX_C_SOURCE
#  else
     /*
      * Else assume that nothing else is needed.  Don't use _POSIX_C_SOURCE on
      * BSD, since it causes anything non-POSIX, such as MAP_ANON, to be hidden.
      */
#    undef _POSIX_C_SOURCE
#  endif
#endif

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "../common_defs.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <sys/types.h>
#endif

#if defined(__GNUC__) || __has_attribute(format)
# define _printf(str_idx, args_idx)	\
		__attribute__((format(printf, str_idx, args_idx)))
#else
# define _printf(str_idx, args_idx)
#endif

#ifdef _WIN32

/*
 * Definitions for Windows builds.  Mainly, 'tchar' is defined to be the 2-byte
 * 'wchar_t' type instead of 'char'.  This is the only "easy" way I know of to
 * get full Unicode support on Windows...
 */

#include <io.h>
#include <wchar.h>
int wmain(int argc, wchar_t **argv);
#  define	tmain		wmain
#  define	tchar		wchar_t
#  define	_T(text)	L##text
#  define	T(text)		_T(text)
#  define	TS		"ls"
#  define	TC		"lc"
#  define	tmemcpy		wmemcpy
#  define	topen		_wopen
#  define	tstrchr		wcschr
#  define	tstrcmp		wcscmp
#  define	tstrlen		wcslen
#  define	tstrrchr	wcsrchr
#  define	tstrtoul	wcstoul
#  define	tstrxcmp	wcsicmp
#  define	tunlink		_wunlink
#  define	tutimbuf	__utimbuf64
#  define	tutime		_wutime64
#  define	tstat		_wstat64
#  define	tfstat		_fstat64
#  define	stat_t		struct _stat64
#  ifdef _MSC_VER
#    define	STDIN_FILENO	0
#    define	STDOUT_FILENO	1
#    define	STDERR_FILENO	2
#    define	S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#    define	S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#  endif

#else /* _WIN32 */

/* Standard definitions for everyone else */

#  define	tmain		main
#  define	tchar		char
#  define	T(text)		text
#  define	TS		"s"
#  define	TC		"c"
#  define	tmemcpy		memcpy
#  define	topen		open
#  define	tstrchr		strchr
#  define	tstrcmp		strcmp
#  define	tstrlen		strlen
#  define	tstrrchr	strrchr
#  define	tstrtoul	strtoul
#  define	tstrxcmp	strcmp
#  define	tunlink		unlink
#  define	tutimbuf	utimbuf
#  define	tutime		utime
#  define	tstat		stat
#  define	tfstat		fstat
#  define	stat_t		struct stat

#endif /* !_WIN32 */

extern const tchar *prog_invocation_name;
extern bool suppress_warnings;

void _printf(1, 2) msg(const char *fmt, ...);
void _printf(1, 2) msg_errno(const char *fmt, ...);
void _printf(1, 2) warn(const char *fmt, ...);

void *xmalloc(size_t size);

void begin_program(tchar *argv[]);

struct file_stream {
	int fd;
	tchar *name;
	bool is_standard_stream;
	void *mmap_token;
	void *mmap_mem;
	size_t mmap_size;
};

int xopen_for_read(const tchar *path, bool symlink_ok,
		   struct file_stream *strm);
int xopen_for_write(const tchar *path, bool force, struct file_stream *strm);
int map_file_contents(struct file_stream *strm, u64 size);

ssize_t xread(struct file_stream *strm, void *buf, size_t count);
int full_write(struct file_stream *strm, const void *buf, size_t count);

int xclose(struct file_stream *strm);

int parse_compression_level(tchar opt_char, const tchar *arg);

struct libdeflate_compressor *alloc_compressor(int level);
struct libdeflate_decompressor *alloc_decompressor(void);

/* tgetopt.c */

extern tchar *toptarg;
extern int toptind, topterr, toptopt;

int tgetopt(int argc, tchar *argv[], const tchar *optstring);

#endif /* PROGRAMS_PROG_UTIL_H */
