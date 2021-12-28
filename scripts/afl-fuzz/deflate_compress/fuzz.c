#include <assert.h>
#include <libdeflate.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
	struct libdeflate_decompressor *d;
	struct libdeflate_compressor *c;
	int ret;
	int fd = open(argv[1], O_RDONLY);
	struct stat stbuf;
	unsigned char level;
	assert(fd >= 0);
	ret = fstat(fd, &stbuf);
	assert(!ret);

	if (stbuf.st_size == 0)
		return 0;
	ret = read(fd, &level, 1);
	assert(ret == 1);
	level %= 13;

	char in[stbuf.st_size - 1];
	ret = read(fd, in, sizeof in);
	assert(ret == sizeof in);

	c = libdeflate_alloc_compressor(level);
	d = libdeflate_alloc_decompressor();

	char out[sizeof(in)];
	char checkarray[sizeof(in)];

	size_t csize = libdeflate_deflate_compress(c, in,sizeof in, out, sizeof out);
	if (csize) {
		enum libdeflate_result res;
		res = libdeflate_deflate_decompress(d, out, csize, checkarray, sizeof in, NULL);
		assert(!res);
		assert(!memcmp(in, checkarray, sizeof in));
	}

	libdeflate_free_compressor(c);
	libdeflate_free_decompressor(d);
	return 0;
}
