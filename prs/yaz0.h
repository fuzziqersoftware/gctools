//#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>


int64_t yaz0_decompress_stream(FILE* in, FILE* out, size_t skip_output_bytes,
    size_t stop_after_size);
