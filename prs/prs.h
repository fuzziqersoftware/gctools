#include <stdio.h>


int64_t prs_compress_stream(FILE* src, FILE* dst, int64_t size);
int64_t prs_decompress_stream(FILE* in, FILE* out, int64_t skip_output_bytes,
    int64_t stop_after_size);
