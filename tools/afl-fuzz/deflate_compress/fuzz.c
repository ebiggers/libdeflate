#include <assert.h>
#include <libdeflate.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
	struct deflate_decompressor *d;
	struct deflate_compressor *c;
	int ret;
	int fd = open(argv[1], O_RDONLY);
	struct stat stbuf;
	assert(fd >= 0);
	ret = fstat(fd, &stbuf);
	assert(!ret);

	char in[stbuf.st_size];
	ret = read(fd, in, sizeof in);
	assert(ret == sizeof in);

	c = deflate_alloc_compressor(6);
	d = deflate_alloc_decompressor();

	char out[sizeof(in)];
	char checkarray[sizeof(in)];

	size_t csize = deflate_compress(c, in,sizeof in, out, sizeof out);
	if (csize) {
		enum decompress_result res;
		res = deflate_decompress(d, out, csize, checkarray, sizeof in, NULL);
		assert(!res);
		assert(!memcmp(in, checkarray, sizeof in));
	}

	deflate_free_compressor(c);
	deflate_free_decompressor(d);
	return 0;
}
