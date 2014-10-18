#include <stdio.h>


int64_t prs_compress_stream(FILE* src, FILE* dst, size_t size);
int64_t prs_decompress_stream(FILE* in, FILE* out, int skip_output_bytes,
    int stop_after_size);
