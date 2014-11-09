//#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>


int64_t yay0_decompress(void* _in, void** _out, int64_t skip_output_bytes,
    int64_t stop_after_size);
int64_t yay0_decompress_stream(FILE* in, FILE* out, int64_t skip_output_bytes,
    int64_t stop_after_size);
