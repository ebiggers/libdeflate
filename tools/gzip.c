/*
 * gzip.c - a gzip-like command line tool that uses libdeflate
 *
 * Author:	Eric Biggers
 * Year:	2016
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#include "util.h"

static bool
is_gunzip(const tchar *argv0)
{
	const tchar *name = basename(argv0);

	if (!tstrcmp(name, T("gunzip")))
		return true;
#ifdef _WIN32
	if (!tstrcmp(name, T("gunzip.exe")))
		return true;
#endif
	return false;
}

static void
usage(void)
{
	fprintf(stderr,
"Usage: gzip [-d] [-f] [-LEVEL] FILE...\n"
"    LEVEL may be 1 through 12.\n"
"    Note: this program is *not* intended as a full gzip replacement and only\n"
"    works well on small to medium-sized files.\n");
	exit(1);
}

static void *
map_file_contents(const tchar *path, size_t *size_ret, void **token_ret)
{
	int fd;
	struct stat stbuf;
	void *map;

	fd = topen(path, O_RDONLY | O_BINARY);

	ASSERT(fd >= 0,
	       "Unable to open file \"%"TS"\" for reading: %s",
	       path, strerror(errno));

	ASSERT(!fstat(fd, &stbuf),
	       "Unable to read metadata of file \"%"TS"\": %s",
	       path, strerror(errno));

	ASSERT(S_ISREG(stbuf.st_mode),
	       "File \"%"TS"\" is not a regular file.", path);

	ASSERT(stbuf.st_size <= SIZE_MAX,
	       "File \"%"TS"\" cannot be processed by "
	       "this program because it is too large.", path);

#ifdef _WIN32
	HANDLE h = CreateFileMapping((HANDLE)(intptr_t)_get_osfhandle(fd),
				     NULL, PAGE_READONLY, 0, 0, NULL);
	ASSERT(h != NULL, "Unable create file mapping for \"%"TS"\": "
	       "Windows error %u", path, (unsigned int )GetLastError());

	map = MapViewOfFile(h, FILE_MAP_READ, 0, 0, stbuf.st_size);
	ASSERT(map != NULL, "Unable to map file \"%"TS"\" into memory: "
	       "Windows error %u", path, (unsigned int)GetLastError());
	*token_ret = h;
#else
	map = mmap(NULL, stbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ASSERT(errno != ENOMEM,
		       "File \"%"TS"\" cannot be processed by "
		       "this program because it is too large.", path);
		fatal_error("Unable to map file \"%"TS"\" into memory: %s",
			    path, strerror(errno));
	}
	*token_ret = NULL;
#endif

	close(fd);
	*size_ret = stbuf.st_size;
	return map;
}

static void
unmap_file_contents(void *token, void *map, size_t size)
{
#ifdef _WIN32
	UnmapViewOfFile(map);
	CloseHandle((HANDLE)token);
#else
	munmap(map, size);
#endif
}

static bool
file_exists(const tchar *path)
{
	struct stat stbuf;
	return !tstat(path, &stbuf);
}

static void
write_file(const tchar *path, const void *contents, size_t size)
{
	int fd;

	fd = topen(path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, 0644);
	ASSERT(fd >= 0, "Unable to open file \"%"TS"\" for writing: %s",
	       path, strerror(errno));

	while (size) {
		int ret = write(fd, contents, size > INT_MAX ? INT_MAX : size);
		ASSERT(ret > 0, "Error writing data to \"%"TS"\": %s",
		       path, strerror(errno));
		size -= ret;
		contents = (const uint8_t *)contents + ret;
	}

	ASSERT(!close(fd), "Error writing data to \"%"TS"\"",
	       path, strerror(errno));
}

static uint32_t
load_u32_gzip(const uint8_t *p)
{
	return ((uint32_t)p[0] << 0) | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
decompress_file(struct deflate_decompressor *d, const tchar *path, bool force)
{
	const tchar *dot;
	tchar *newpath;
	void *map_token;
	void *compressed_data;
	size_t compressed_size;
	void *uncompressed_data;
	size_t uncompressed_size;
	enum decompress_result result;

	if (!(dot = tstrrchr(path, '.')) ||
	    (tstrcmp(dot, T(".gz")) && tstrcmp(dot, T(".GZ"))))
	{
		fatal_error("File \"%"TS"\" does not end with the .gz "
			    "extension.", path);
	}

	newpath = malloc((dot + 1 - path) * sizeof(tchar));
	ASSERT(newpath != NULL, "Out of memory");
	tmemcpy(newpath, path, dot - path);
	newpath[dot - path] = '\0';

	ASSERT(!file_exists(newpath) || force,
	       "File \"%"TS"\" already exists.  "
	       "Use the -f option if you want to overwrite it.", newpath);

	compressed_data = map_file_contents(path, &compressed_size, &map_token);

	ASSERT(compressed_size >= sizeof(uint32_t),
	       "File \"%"TS"\" is not a gzip file.", path);
	uncompressed_size = load_u32_gzip((const uint8_t *)compressed_data +
					  (compressed_size - sizeof(uint32_t)));
	uncompressed_data = malloc(uncompressed_size);
	ASSERT(uncompressed_data != NULL,
	       "Insufficient memory to allocate buffer for uncompressed "
	       "data of file \"%"TS"\".\n"
	       "       The file is probably too large to be processed by "
	       "this program.", path);

	result = gzip_decompress(d, compressed_data, compressed_size,
				 uncompressed_data, uncompressed_size, NULL);
	ASSERT(result != DECOMPRESS_INSUFFICIENT_SPACE,
	       "Decompression of \"%"TS"\" failed with error code: "
	       "INSUFFICIENT_SPACE.\n",
	       "       The file is probably too large to be processed by "
	       "this program.", path);

	ASSERT(result == DECOMPRESS_SUCCESS,
	       "Decompression of \"%"TS"\" failed with error code: %d.\n",
	       result);

	write_file(newpath, uncompressed_data, uncompressed_size);

	free(uncompressed_data);
	unmap_file_contents(map_token, compressed_data, compressed_size);
	free(newpath);

	tunlink(path);
}

static void
compress_file(struct deflate_compressor *c, const tchar *path, bool force)
{
	size_t path_nchars = tstrlen(path);
	tchar *newpath;
	void *map_token;
	void *uncompressed_data;
	size_t uncompressed_size;
	void *compressed_data;
	size_t compressed_size;

	newpath = malloc((path_nchars + 4) * sizeof(tchar));
	ASSERT(newpath != NULL, "Out of memory");
	tmemcpy(newpath, path, path_nchars);
	tmemcpy(&newpath[path_nchars], T(".gz"), 4);

	ASSERT(!file_exists(newpath) || force,
	       "File \"%"TS"\" already exists.  "
	       "Use the -f option if you want to overwrite it.", newpath);

	uncompressed_data = map_file_contents(path, &uncompressed_size,
					      &map_token);

	compressed_size = gzip_compress_bound(c, uncompressed_size);
	compressed_data = malloc(compressed_size);
	ASSERT(compressed_data != NULL,
	       "Insufficient memory to allocate buffer for compressed data "
	       "of file \"%"TS"\".\n"
	       "       The file is probably too large to be processed by "
	       "this program.", path);

	compressed_size = gzip_compress(c, uncompressed_data, uncompressed_size,
					compressed_data, compressed_size);
	ASSERT(compressed_size != 0,
	       "gzip_compress() returned a compressed size of 0. "
	       "This should not happen.");

	write_file(newpath, compressed_data, compressed_size);

	free(compressed_data);
	unmap_file_contents(map_token, uncompressed_data, uncompressed_size);
	free(newpath);

	tunlink(path);
}

int
main(int argc, tchar *argv[])
{
	bool decompress = is_gunzip(argv[0]);
	int num_files = 0;
	int level = 6;
	bool force = false;

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (argv[i][1] == 'd') {
				if (argv[i][2] != '\0')
					usage();
				decompress = true;
			} else if (argv[i][1] == 'f') {
				if (argv[i][2] != '\0')
					usage();
				force = true;
			} else {
				level = tstrtol(&argv[i][1], NULL, 10);
				if (level < 1 || level > 12)
					usage();
			}
		} else {
			argv[num_files++] = argv[i];
		}
	}

	if (num_files == 0)
		usage();

	if (decompress) {
		struct deflate_decompressor *d;

		d = deflate_alloc_decompressor();
		ASSERT(d != NULL, "Unable to allocate decompressor");

		for (int i = 0; i < num_files; i++)
			decompress_file(d, argv[i], force);

		deflate_free_decompressor(d);
	} else {
		struct deflate_compressor *c;

		c = deflate_alloc_compressor(level);
		ASSERT(c != NULL, "Unable to allocate compressor");

		for (int i = 0; i < num_files; i++)
			compress_file(c, argv[i], force);

		deflate_free_compressor(c);
	}

	return 0;
}
