/*
 * gzip_decompress_by_stream.h
 * added decompress by stream, 2023 housisong
 */
#ifndef PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H
#define PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
#include "prog_util.h"

enum libdeflate_destream_result{
	LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR =20,
	LIBDEFLATE_DESTREAM_READ_FILE_ERROR,
	LIBDEFLATE_DESTREAM_WRITE_FILE_ERROR,
	LIBDEFLATE_DESTREAM_DATA_CRC_ERROR,
	LIBDEFLATE_DESTREAM_DATA_SIZE_ERROR,
};

//decompress gzip by stream
//  'out','actual_in_nbytes_ret','actual_out_nbytes_ret' can NULL;
//	return value is libdeflate_result or libdeflate_destream_result.
int gzip_decompress_by_stream(struct libdeflate_decompressor *decompressor,
							  struct file_stream *in, u64 in_size, struct file_stream *out,
							  u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret);

#ifdef __cplusplus
}
#endif
#endif /* PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H */
