#include <stdio.h>
#include <stdint.h>



size_t prs_compress_stream(FILE* src, FILE* dst, int64_t size);
size_t prs_decompress_stream(FILE* in, FILE* out, int64_t stop_after_size);
