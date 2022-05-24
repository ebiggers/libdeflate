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
	unsigned char use_bound;

	assert(fd >= 0);
	ret = fstat(fd, &stbuf);
	assert(!ret);

	if (stbuf.st_size < 2)
		return 0;
	ret = read(fd, &level, 1);
	assert(ret == 1);
	level %= 13;

	ret = read(fd, &use_bound, 1);
	assert(ret == 1);
	use_bound %= 2;

	char in[stbuf.st_size - 2];
	ret = read(fd, in, sizeof in);
	assert(ret == sizeof in);

	c = libdeflate_alloc_compressor(level);
	d = libdeflate_alloc_decompressor();

	size_t outsize = use_bound ? libdeflate_deflate_compress_bound(c, sizeof(in)) : sizeof(in);
	char out[outsize];
	char checkarray[sizeof(in)];

	size_t csize = libdeflate_deflate_compress(c, in,sizeof in, out, outsize);
	if (csize != 0) {
		enum libdeflate_result res;
		res = libdeflate_deflate_decompress(d, out, csize, checkarray, sizeof in, NULL);
		assert(!res);
		assert(!memcmp(in, checkarray, sizeof in));
	} else {
		assert(!use_bound);
	}

	libdeflate_free_compressor(c);
	libdeflate_free_decompressor(d);
	return 0;
}
