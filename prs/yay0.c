#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "errors.h"
#include "util.h"


struct yay0_header {
  char magic[4]; // Yay0
  uint32_t uncompressed_size; // total size of uncompressed data
  uint32_t count_offset;
  uint32_t data_offset;
} __attribute__ ((packed));


int64_t yay0_decompress(void* _in, void** _out, int64_t stop_after_size) {

  struct yay0_header* header = (struct yay0_header*)_in;
  if (header->magic[0] != 'Y' || header->magic[1] != 'a' ||
      header->magic[2] != 'y' || header->magic[3] != '0') {
    return ERROR_UNRECOGNIZED;
  }

  uint32_t total_size = byteswap32(header->uncompressed_size);
  if (total_size == 0) {
    return 0;
  }
  if (stop_after_size && total_size > stop_after_size) {
    total_size = stop_after_size;
  }

  *_out = malloc(total_size);
  if (!*_out) {
    return ERROR_MEMORY;
  }

  uint32_t count_offset = byteswap32(header->count_offset);
  uint32_t data_offset = byteswap32(header->data_offset);

  uint8_t* control_stream = (uint8_t*)(header + 1);
  uint8_t* count_stream = (uint8_t*)_in + count_offset;
  uint8_t* data_stream = (uint8_t*)_in + data_offset;
  uint8_t* out_stream = (uint8_t*)*_out;

  uint32_t bytes_written = 0;
  uint8_t control_bits_remaining = 0;
  uint8_t control_byte;

  while (bytes_written < total_size) {

    if (control_bits_remaining == 0) {
      control_byte = *(control_stream++);
      control_bits_remaining = 8;
    }

    if ((control_byte & 0x80) != 0) {
      out_stream[bytes_written] = *(data_stream++);
      bytes_written++;

    } else {
      uint16_t nr = (*(count_stream++) << 8);
      nr |= *(count_stream++);
      uint16_t r = (nr & 0x0FFF) + 1;
      uint16_t n = (nr & 0xF000) >> 12;
      if (n == 0) {
        n = *(data_stream++) + 0x12; // TODO is this really read from the data stream? (not the count stream?)
      } else {
        n += 2;
      }

      if (r > bytes_written) {
        return ERROR_BACKREFERENCE_TOO_DISTANT;
      }
      if (bytes_written + n > total_size && stop_after_size == total_size) {
        return ERROR_OVERFLOW;
      }

      for (; n > 0; n--) {
        out_stream[bytes_written] = out_stream[bytes_written - r];
        bytes_written++;
      }
    }

    control_byte <<= 1;
    control_bits_remaining--;    
  }

  return bytes_written;
}

int64_t yay0_decompress_stream(FILE* in, FILE* out, int64_t stop_after_size) {

  void* input_data;
  int64_t input_size = read_entire_stream(in, &input_data);
  if (input_size < 0) {
    return input_size;
  }

  void* output_data = NULL;
  int64_t ret = yay0_decompress(input_data, &output_data, stop_after_size);
  if (ret > 0) {
    fwrite(output_data, 1, ret, out);
    free(output_data);
  }

  free(input_data);
  return ret;
}
