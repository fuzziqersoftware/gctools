#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

int32_t byteswap32(int32_t a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00) |
         ((a << 8) & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

int64_t read_entire_stream(FILE* f, void** _data) {
  int64_t alloc_size = 0x4000;
  int64_t data_size = 0;
  *_data = malloc(alloc_size);

  do {
    if (data_size == alloc_size) {
      alloc_size *= 2;
      *_data = realloc(*_data, alloc_size);
    }

    int64_t this_read_size = alloc_size - data_size;
    int64_t bytes_read = fread(((uint8_t*)*_data) + data_size, 1, this_read_size, f);
    data_size += bytes_read;
  } while (!feof(f) && !ferror(f));

  if (ferror(f)) {
    free(*_data);
    *_data = NULL;
    return -1;
  }

  return data_size;
}
