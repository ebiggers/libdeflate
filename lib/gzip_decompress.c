/*
 * gzip_decompress.c - decompress with a gzip wrapper
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

#include "gzip_overhead.h"
#include "gzip_constants.h"

int libdeflate_gzip_decompress_head(const void *in, size_t in_nbytes,
			      size_t *actual_in_nbytes_ret)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u8 flg;

	if (in_nbytes < GZIP_MIN_OVERHEAD)
		return LIBDEFLATE_BAD_DATA;

	/* ID1 */
	if (*in_next++ != GZIP_ID1)
		return LIBDEFLATE_BAD_DATA;
	/* ID2 */
	if (*in_next++ != GZIP_ID2)
		return LIBDEFLATE_BAD_DATA;
	/* CM */
	if (*in_next++ != GZIP_CM_DEFLATE)
		return LIBDEFLATE_BAD_DATA;
	flg = *in_next++;
	/* MTIME */
	in_next += 4;
	/* XFL */
	in_next += 1;
	/* OS */
	in_next += 1;

	if (flg & GZIP_FRESERVED)
		return LIBDEFLATE_BAD_DATA;

	/* Extra field */
	if (flg & GZIP_FEXTRA) {
		u16 xlen = get_unaligned_le16(in_next);
		in_next += 2;

		if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;

		in_next += xlen;
	}

	/* Original file name (zero terminated) */
	if (flg & GZIP_FNAME) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	/* File comment (zero terminated) */
	if (flg & GZIP_FCOMMENT) {
		while (*in_next++ != 0 && in_next != in_end)
			;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	/* CRC16 for gzip header */
	if (flg & GZIP_FHCRC) {
		in_next += 2;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return LIBDEFLATE_BAD_DATA;
	}

	if (actual_in_nbytes_ret)
		*actual_in_nbytes_ret = in_next - (u8 *)in;

	return LIBDEFLATE_SUCCESS;
}


#define _do_decompress_step(_call_decompress) {	\
	result=_call_decompress; 			\
	if (result != LIBDEFLATE_SUCCESS)	\
		return result;					\
	in_next += actual_in_nbytes;		\
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_gzip_decompress_ex(struct libdeflate_decompressor *d,
			      const void *in, size_t in_nbytes,
			      void *out, size_t out_nbytes_avail,
			      size_t *actual_in_nbytes_ret,
			      size_t *actual_out_nbytes_ret)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	size_t actual_in_nbytes;
	size_t actual_out_nbytes;
	enum libdeflate_result result;
	u32  saved_crc;
	u32  saved_uncompress_nbytes;

	_do_decompress_step(libdeflate_gzip_decompress_head(in_next,
					in_end - in_next, &actual_in_nbytes));
	
	/* Compressed data  */
	_do_decompress_step(libdeflate_deflate_decompress_ex(d, in_next,
					in_end - GZIP_FOOTER_SIZE - in_next,
					out, out_nbytes_avail,
					&actual_in_nbytes,
					&actual_out_nbytes));
	if (actual_out_nbytes_ret)
		*actual_out_nbytes_ret=actual_out_nbytes;

	_do_decompress_step(libdeflate_gzip_decompress_foot(in_next, in_end - in_next,
					&saved_crc, &saved_uncompress_nbytes, &actual_in_nbytes));
	
	/* CRC32 */
	if (libdeflate_crc32(0, out, actual_out_nbytes) != saved_crc)
		return LIBDEFLATE_BAD_DATA;

	/* ISIZE */
	if ((u32)actual_out_nbytes != saved_uncompress_nbytes)
		return LIBDEFLATE_BAD_DATA;

	if (actual_in_nbytes_ret)
		*actual_in_nbytes_ret = in_next - (u8 *)in;

	return LIBDEFLATE_SUCCESS;
}

int libdeflate_gzip_decompress_foot(const void *in, size_t in_nbytes,
				  u32* saved_crc,u32* saved_uncompress_nbytes,
				  size_t *actual_in_nbytes_ret)
{
	const u8 *in_next = in;
	if (in_nbytes < GZIP_FOOTER_SIZE)
		return LIBDEFLATE_BAD_DATA;

	/* CRC32 */
	*saved_crc=get_unaligned_le32(in_next);
	in_next += 4;

	/* ISIZE */
	*saved_uncompress_nbytes=get_unaligned_le32(in_next);
	in_next += 4;

	if (actual_in_nbytes_ret)
		*actual_in_nbytes_ret = in_next - (u8 *)in;

	return LIBDEFLATE_SUCCESS;
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_gzip_decompress(struct libdeflate_decompressor *d,
			   const void *in, size_t in_nbytes,
			   void *out, size_t out_nbytes_avail,
			   size_t *actual_out_nbytes_ret)
{
	return libdeflate_gzip_decompress_ex(d, in, in_nbytes,
					     out, out_nbytes_avail,
					     NULL, actual_out_nbytes_ret);
}
