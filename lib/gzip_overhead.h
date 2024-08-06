#ifndef LIB_GZIP_OVERHEAD_H
#define LIB_GZIP_OVERHEAD_H

#include "lib_common.h"
#ifdef __cplusplus
extern "C" {
#endif
size_t libdeflate_gzip_compress_head(unsigned compression_level,size_t in_nbytes,
			 void *out, size_t out_nbytes_avail);
size_t libdeflate_gzip_compress_foot(uint32_t in_crc, size_t in_nbytes,
			 void *out, size_t out_nbytes_avail);
int libdeflate_gzip_decompress_head(const void *in, size_t in_nbytes,
			 size_t *actual_in_nbytes_ret);
int libdeflate_gzip_decompress_foot(const void *in, size_t in_nbytes,
			 u32* saved_crc,u32* saved_uncompress_nbytes,
			 size_t *actual_in_nbytes_ret);
#ifdef __cplusplus
}
#endif
#endif /* LIB_GZIP_OVERHEAD_H */
