#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include <stdexcept>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

#include "PRSDataLog.hh"

using namespace std;

struct PRSCompressionOutput {
  PRSDataLog forward_log;
  FILE* out;
  int64_t bytes_written;
  uint8_t bitpos;

  PRSCompressionOutput(FILE* out)
    : forward_log(),
      out(out),
      bytes_written(1),
      bitpos(0) {
    this->forward_log.add(0);
  }

  void put_control_bit_nosave(bool bit) {
    this->forward_log.data[0] = static_cast<uint8_t>(this->forward_log.data[0]) >> 1;
    this->forward_log.data[0] |= (bit << 7);
    this->bitpos++;
  }

  void save_control() {
    if (this->bitpos >= 8) {
      this->bitpos = 0;
      fwritex(this->out, this->forward_log.data.data(), this->forward_log.size);
      this->forward_log.size = 0;
      this->forward_log.add(0);
      this->bytes_written++;
    }
  }

  void put_control_bit(bool bit) {
    this->put_control_bit_nosave(bit);
    this->save_control();
  }

  void put_static_data(uint8_t data) {
    this->forward_log.add(data);
    this->bytes_written++;
  }

  void finish() {
    this->put_control_bit(0);
    this->put_control_bit(1);
    if (this->bitpos != 0) {
      this->forward_log.data[0] = ((this->forward_log.data[0] << this->bitpos) >> 8);
    }
    this->put_static_data(0);
    this->put_static_data(0);
    fwritex(this->out, this->forward_log.data.data(), this->forward_log.size);
  }

  void put_raw_byte(uint8_t value) {
    this->put_control_bit_nosave(1);
    this->put_static_data(value);
    this->save_control();
  }

  void put_short_copy(ssize_t offset, uint8_t size) {
    size -= 2;
    this->put_control_bit(0);
    this->put_control_bit(0);
    this->put_control_bit((size >> 1) & 1);
    this->put_control_bit_nosave(size & 1);
    this->put_static_data(offset & 0xFF);
    this->save_control();
  }

  void put_long_copy(ssize_t offset, uint8_t size) {
    if (size <= 9) {
      this->put_control_bit(0);
      this->put_control_bit_nosave(1);
      this->put_static_data(((offset << 3) & 0xF8) | ((size - 2) & 0x07));
      this->put_static_data((offset >> 5) & 0xFF);
      this->save_control();
    } else {
      this->put_control_bit(0);
      this->put_control_bit_nosave(1);
      this->put_static_data((offset << 3) & 0xF8);
      this->put_static_data((offset >> 5) & 0xFF);
      this->put_static_data(size - 1);
      this->save_control();
    }
  }

  void put_copy(ssize_t offset, uint8_t size) {
    if ((offset > -0x100) && (size <= 5)) {
      this->put_short_copy(offset, size);
    } else {
      this->put_long_copy(offset, size);
    }
  }
};

int64_t prs_compress_stream(FILE* src, FILE* dst, int64_t size) {
  PRSDataLog reverse_log;
  PRSDataLog forward_log;
  PRSCompressionOutput pc(dst);

  while (size > 0 || size == -1) {
    forward_log.fill(src, size);
    if (forward_log.offset == forward_log.size) {
      break; // No more data to compress
    }

    // Look in the reverse log for a match for the data at the beginning of the
    // forward log
    ssize_t best_offset = 0;
    ssize_t best_size = 0;
    ssize_t this_offset;
    for (this_offset = -3;
         (this_offset >= -static_cast<ssize_t>(reverse_log.size)) &&
           (this_offset > -0x1FF0) && (best_size < 255);
         this_offset--) {

      // Try to expand the current match as much as possible. Note that we only
      // need to look at the last byte each time through the loop, since we know
      // the bytes before it all already match
      ssize_t this_size = 1;
      while ((reverse_log.data[reverse_log.size + this_offset + this_size - 1] ==
               forward_log.data[forward_log.offset + this_size - 1]) &&
             (this_size < 256) &&
             ((this_offset + this_size) < 0) &&
             (this_size <= (forward_log.size - forward_log.offset))) {
        this_size++;
      }
      this_size--;

      if (this_size > best_size) {
        best_offset = this_offset;
        best_size = this_size;
      }
    }
    if (best_size < 3) {
      pc.put_raw_byte(forward_log.data[forward_log.offset]);
      reverse_log.add(forward_log.data[forward_log.offset]);
      forward_log.offset++;
      if (size > 0) {
        size--;
      }

    } else {
      pc.put_copy(best_offset, best_size);
      reverse_log.add(&forward_log.data[forward_log.offset + best_offset],
          best_size);
      forward_log.offset += best_size;
      if (size > 0) {
        size -= best_size;
      }
    }

    if (forward_log.offset > forward_log.size || forward_log.offset < 0) {
      throw logic_error(string_printf(
          "warning: forward_log has invalid offset (%d %d)\n",
          forward_log.offset, forward_log.size));
    }
  }
  pc.finish();

  return pc.bytes_written;
}



int64_t prs_decompress_stream(FILE* in, FILE* out, int64_t stop_after_size) {
  PRSDataLog log;

  int32_t r3, r5;
  int bitpos = 9;
  uint8_t currentbyte;
  int flag;
  ssize_t offset;
  unsigned long x, t;
  unsigned long out_size = 0;

  currentbyte = fgetcx(in);

  for (;;) {
    bitpos--;
    if (bitpos == 0) {
      currentbyte = fgetcx(in);
      bitpos = 8;
    }
    flag = currentbyte & 1;
    currentbyte = currentbyte >> 1;
    if (flag) {
      uint8_t ch = fgetcx(in);
      fputc(ch, out);
      out_size++;
      if (stop_after_size && (out_size >= stop_after_size)) {
        return out_size;
      }
      log.add(ch);
      continue;
    }
    bitpos--;
    if (bitpos == 0) {
      currentbyte = fgetcx(in);
      bitpos = 8;
    }
    flag = currentbyte & 1;
    currentbyte = currentbyte >> 1;
    if (flag) {
      r3 = fgetcx(in);
      uint8_t high_byte = fgetcx(in);
      offset = ((high_byte & 0xFF) << 8) | (r3 & 0xFF);
      if (offset == 0) {
        return out_size;
      }
      r3 = r3 & 0x00000007;
      r5 = (offset >> 3) | 0xFFFFE000;
      if (r3 == 0) {
        flag = 0;
        r3 = fgetcx(in);
        r3 = (r3 & 0xFF) + 1;
      } else {
        r3 += 2;
      }
    } else {
      r3 = 0;
      for (x = 0; x < 2; x++) {
        bitpos--;
        if (bitpos == 0) {
          currentbyte = fgetcx(in);
          bitpos = 8;
        }
        flag = currentbyte & 1;
        currentbyte = currentbyte >> 1;
        offset = r3 << 1;
        r3 = offset | flag;
      }
      offset = fgetcx(in);
      r3 += 2;
      r5 = offset | 0xFFFFFF00;
    }
    if (r3 == 0) {
      continue;
    }
    t = r3;
    for (x = 0; x < t; x++) {
      uint8_t ch = log.data[log.size + r5];
      fputc(ch, out);
      out_size++;
      if (stop_after_size && (out_size >= stop_after_size)) {
        return out_size;
      }
      log.add(ch);
    }
  }
}
