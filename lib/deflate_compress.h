#ifndef _LIB_DEFLATE_COMPRESS_H
#define _LIB_DEFLATE_COMPRESS_H

/* DEFLATE compression is private to deflate_compress.c, but we do need to be
 * able to query the compression level for zlib and gzip header generation.  */

struct deflate_compressor;

extern unsigned int deflate_get_compression_level(struct deflate_compressor *c);

#endif /* _LIB_DEFLATE_COMPRESS_H */
