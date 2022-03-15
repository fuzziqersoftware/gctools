//#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <stdexcept>
#include <phosg/Filesystem.hh>
#include <phosg/Encoding.hh>

#include "PRSDataLog.hh"

using namespace std;


struct Yaz0Header {
  char magic[4]; // Yaz0
  be_uint32_t uncompressed_size; // total size of uncompressed data
  char padding[8];
} __attribute__ ((packed));


size_t yaz0_decompress_stream(FILE* in, FILE* out, size_t max_out_size) {
  Yaz0Header header;
  freadx(in, &header, sizeof(Yaz0Header));
  if (header.magic[0] != 'Y' || header.magic[1] != 'a' ||
      header.magic[2] != 'z' || header.magic[3] != '0') {
    throw runtime_error("input is not Yaz0-compressed");
  }

  uint32_t total_size = header.uncompressed_size;
  if (total_size == 0) {
    return 0;
  }
  if (max_out_size && total_size > max_out_size) {
    total_size = max_out_size;
  }

  PRSDataLog log;

  size_t bytes_written = 0;
  uint8_t control_bits_remaining = 0;
  uint8_t control_byte;

  while (bytes_written < total_size) {
    if (control_bits_remaining == 0) {
      control_byte = fgetcx(in);
      control_bits_remaining = 8;
    }

    if (control_byte & 0x80) {
      int ch = fgetc(in);
      fputc(ch, out);
      log.add(ch);
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

      if (r > static_cast<ssize_t>(bytes_written)) {
        throw runtime_error("backreference beyond beginning of output");
      }
      if (max_out_size && (bytes_written + n > total_size) && (max_out_size == total_size)) {
        throw runtime_error("output overflows maximum output size");
      }

      for (; n > 0; n--) {
        fputc(log.data[log.size - r], out);
        log.add(log.data[log.size - r]);
        bytes_written++;
      }
    }

    control_byte <<= 1;
    control_bits_remaining--;
  }

  return bytes_written;
}
