#include <stdio.h>
#include <stdint.h>



size_t prs_compress_stream(FILE* src, FILE* dst, size_t size);
size_t prs_decompress_stream(FILE* in, FILE* out, size_t stop_after_size);
