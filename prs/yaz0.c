//#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>


#include "data_log.h"
#include "errors.h"
#include "util.h"


struct yaz0_header {
  char magic[4]; // Yaz0
  uint32_t uncompressed_size; // total size of uncompressed data
  char padding[8];
} __attribute__ ((packed));


int64_t yaz0_decompress_stream(FILE* in, FILE* out, int64_t stop_after_size) {

  struct yaz0_header header;
  fread(&header, sizeof(struct yaz0_header), 1, in);
  if (header.magic[0] != 'Y' || header.magic[1] != 'a' ||
      header.magic[2] != 'z' || header.magic[3] != '0') {
    return ERROR_UNRECOGNIZED;
  }

  uint32_t total_size = byteswap32(header.uncompressed_size);
  if (total_size == 0) {
    return 0;
  }
  if (stop_after_size && total_size > stop_after_size) {
    total_size = stop_after_size;
  }

  struct data_log log;
  log_init(&log);

  uint32_t bytes_written = 0;
  uint8_t control_bits_remaining = 0;
  uint8_t control_byte;

  while (bytes_written < total_size) {
    if (control_bits_remaining == 0) {
      control_byte = fgetc(in);
      control_bits_remaining = 8;
    }

    if (control_byte & 0x80) {
      int ch = fgetc(in);
      fputc(ch, out);
      log_byte(&log, ch);
      bytes_written++;

    } else {
      uint16_t nr = (fgetc(in) << 8);
      nr |= fgetc(in);
      int16_t n, r = (nr & 0x0FFF) + 1;
      if ((nr & 0xF000) == 0) {
        n = fgetc(in) + 0x12;
      } else {
        n = ((nr & 0xF000) >> 12) + 2;
      }

      if (r > bytes_written) {
        delete_log(&log);
        return ERROR_BACKREFERENCE_TOO_DISTANT;
      }
      if (bytes_written + n > total_size && stop_after_size == total_size) {
        delete_log(&log);
        return ERROR_OVERFLOW;
      }

      for (; n > 0; n--) {
        fputc(log.data[log.size - r], out);
        log_byte(&log, log.data[log.size - r]);
        bytes_written++;
      }
    }

    control_byte <<= 1;
    control_bits_remaining--;
  }

  delete_log(&log);
  return bytes_written;
}
