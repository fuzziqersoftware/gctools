#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <phosg/Encoding.hh>
#include <phosg/Strings.hh>

using namespace std;

struct Yay0Header {
  char magic[4]; // 'Yay0'
  phosg::be_uint32_t uncompressed_size;
  phosg::be_uint32_t count_offset;
  phosg::be_uint32_t data_offset;
} __attribute__((packed));

string yay0_decompress(const void* in_data, size_t in_size, size_t max_out_size) {
  phosg::StringReader r(in_data, in_size);
  Yay0Header header = r.get<Yay0Header>();
  if (header.magic[0] != 'Y' || header.magic[1] != 'a' ||
      header.magic[2] != 'y' || header.magic[3] != '0') {
    throw runtime_error("input is not Yay0-compressed");
  }

  uint32_t total_size = header.uncompressed_size;
  if (total_size == 0) {
    return 0;
  }
  if (max_out_size && total_size > max_out_size) {
    total_size = max_out_size;
  }

  string ret;
  ret.reserve(total_size);

  phosg::StringReader control_stream_r = r;
  control_stream_r.go(sizeof(Yay0Header));
  phosg::StringReader count_stream_r = r;
  count_stream_r.go(header.count_offset);
  phosg::StringReader data_stream_r = r;
  data_stream_r.go(header.data_offset);

  uint8_t control_bits_remaining = 0;
  uint8_t control_byte;
  while (ret.size() < total_size) {

    if (control_bits_remaining == 0) {
      control_byte = control_stream_r.get_u8();
      control_bits_remaining = 8;
    }

    if ((control_byte & 0x80) != 0) {
      ret.push_back(data_stream_r.get_u8());

    } else {
      uint16_t nr = count_stream_r.get_u16b();
      uint16_t r = (nr & 0x0FFF) + 1;
      uint16_t n = (nr & 0xF000) >> 12;
      if (n == 0) {
        // TODO: is this really read from the data stream and not the count stream?
        n = data_stream_r.get_u8() + 0x12;
      } else {
        n += 2;
      }

      if (r > ret.size()) {
        throw runtime_error("backreference beyond beginning of output");
      }
      if (max_out_size && (ret.size() + n > total_size) && (max_out_size == total_size)) {
        throw runtime_error("output overflows maximum output size");
      }

      for (; n > 0; n--) {
        ret.push_back(ret[ret.size() - r]);
      }
    }

    control_byte <<= 1;
    control_bits_remaining--;
  }

  return ret;
}
