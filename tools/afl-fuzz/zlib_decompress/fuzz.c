#include <assert.h>
#include <libdeflate.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
	struct deflate_decompressor *d;
	int ret;
	int fd = open(argv[1], O_RDONLY);
	struct stat stbuf;
	assert(fd >= 0);
	ret = fstat(fd, &stbuf);
	assert(!ret);

	char in[stbuf.st_size];
	ret = read(fd, in, sizeof in);
	assert(ret == sizeof in);

	char out[sizeof(in) * 3];

	d = deflate_alloc_decompressor();

	zlib_decompress(d, in, sizeof in, out, sizeof out, NULL);
	deflate_free_decompressor(d);
	return 0;
}
