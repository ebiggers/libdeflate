/*
 * gzip_compress_by_stream.c
 * added compress by stream, 2023--2024 housisong
 */
#include "../lib/gzip_overhead.h"
#include "gzip_compress_by_stream.h"
#include <assert.h>

static const size_t kDictSize  = (1<<15); //MATCHFINDER_WINDOW_SIZE
static const size_t kStepSize_better= (size_t)1024*1024*2;
static const size_t kStepSize_min =(size_t)1024*256;
#define _check(v,_ret_errCode) do { if (!(v)) {err_code=_ret_errCode; goto _out; } } while (0)
static inline size_t _dictSize_avail(u64 uncompressed_pos) {
                        return (uncompressed_pos<kDictSize)?(size_t)uncompressed_pos:kDictSize; }

static inline size_t _limitStepSize(u64 in_size,size_t in_step_size){
    if (unlikely(in_step_size>in_size)) in_step_size=in_size;
    if (unlikely(in_step_size<kStepSize_min)) in_step_size=kStepSize_min;
    return in_step_size;
}

int gzip_compress_by_stream(int compression_level,struct file_stream *in,u64 in_size,
                               struct file_stream *out,u64* actual_out_nbytes_ret){
    int err_code=0;
    u8* pmem=0;
    struct libdeflate_compressor* c=0;
    uint32_t     in_crc=0;
    u64 out_cur=0;
    size_t in_step_size=_limitStepSize(in_size,kStepSize_better);
    const size_t block_bound=libdeflate_deflate_compress_bound_block(in_step_size);
    size_t one_buf_size=kDictSize+in_step_size+block_bound;

    pmem=(u8*)malloc(one_buf_size);
    _check(pmem!=0, LIBDEFLATE_ENSTREAM_MEM_ALLOC_ERROR);
    c=libdeflate_alloc_compressor(compression_level);
    _check(c!=0, LIBDEFLATE_ENSTREAM_ALLOC_COMPRESSOR_ERROR);

    {//gzip head
        size_t code_nbytes=libdeflate_gzip_compress_head(compression_level,in_size,pmem,block_bound);
        _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_GZIP_HEAD_ERROR);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
        out_cur+=code_nbytes;
    }

    { // compress blocks single thread; you can compress blocks with multi-thread (set is_byte_align=1);
        const int is_byte_align = 0;
        u8* pdata=pmem;
        u8* pcode=pdata+kDictSize+in_step_size;
        for (u64 in_cur=0;in_cur<in_size;){//compress by stream
            bool is_final_block=(in_cur+in_step_size>=in_size);
            size_t in_nbytes=is_final_block?in_size-in_cur:in_step_size;
            size_t dict_size=_dictSize_avail(in_cur);

            //read block data
            ssize_t r_len=xread(in,pdata+dict_size,in_nbytes);
            _check(r_len==in_nbytes, LIBDEFLATE_ENSTREAM_READ_FILE_ERROR);
            in_crc=libdeflate_crc32(in_crc,pdata+dict_size,in_nbytes);

            //compress the block
            size_t code_nbytes=libdeflate_deflate_compress_block(c,pdata,dict_size,in_nbytes,is_final_block,
                                                                 pcode,block_bound,is_byte_align);
            _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_COMPRESS_BLOCK_ERROR);

            //write the block's code
            int w_ret=full_write(out,pcode,code_nbytes);
            _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
            out_cur+=code_nbytes;

            //dict data for next block
            in_cur+=in_nbytes;
            size_t nextDictSize=_dictSize_avail(in_cur);
            memmove(pdata,pdata+dict_size+in_nbytes-nextDictSize,nextDictSize);
        }
    }
    
    {//gzip foot
        size_t code_nbytes=libdeflate_gzip_compress_foot(in_crc,in_size,pmem,block_bound);
        _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_GZIP_FOOT_ERROR);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
        out_cur+=code_nbytes;
    }

    if (actual_out_nbytes_ret)
        *actual_out_nbytes_ret=out_cur;

_out:
    if (c) libdeflate_free_compressor(c);
    if (pmem) free(pmem);
    return err_code;
}

