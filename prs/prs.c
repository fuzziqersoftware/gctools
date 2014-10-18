#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "errors.h"
#include "util.h"



struct data_log {
  uint8_t* data;
  int size;
  int offset;
};

static void log_init(struct data_log* log) {
  log->data = malloc(0x8000);
  log->size = 0;
  log->offset = 0;
}

static void log_byte(struct data_log* log, int ch) {
  if (log->size == 0x8000) {
    memmove(log->data, &log->data[0x4000], 0x4000);
    log->size -= 0x4000;
    log->offset -= 0x4000;
  }
  log->data[log->size] = ch;
  log->size++;
}

static void log_bytes(struct data_log* log, void* data, size_t size) {
  uint8_t* bytes = (uint8_t*)data;
  for (; size > 0; bytes++, size--)
    log_byte(log, *bytes);
}

static void delete_log(struct data_log* log) {
  if (log->data)
    free(log->data);
}

static void fill_log(struct data_log* log, FILE* f, size_t max_read) {
  if (log->offset > 0x6000) {
    log->offset -= 0x2000;
    log->size -= 0x2000;
    memmove(log->data, &log->data[0x2000], log->size);
  }
  if (log->size < 0x8000) {
    int bytes_to_read = 0x8000 - log->size;
    if (bytes_to_read > max_read)
      bytes_to_read = max_read;
    if (bytes_to_read) {
      int bytes_read = fread(&log->data[0x8000 - bytes_to_read], 1, bytes_to_read, f);
      log->size += bytes_read;
    }
  }
}



struct prs_compress_ctx {
  unsigned char bitpos;
  struct data_log forward_log;
  FILE* out;
};

static void prs_put_control_bit_nosave(struct prs_compress_ctx* pc, unsigned char bit) {
  pc->forward_log.data[0] = pc->forward_log.data[0] >> 1;
  pc->forward_log.data[0] |= ((!!bit) << 7);
  pc->bitpos++;
}

static void prs_put_control_save(struct prs_compress_ctx* pc) {
  if (pc->bitpos >= 8) {
    pc->bitpos = 0;
    fwrite(pc->forward_log.data, pc->forward_log.size, 1, pc->out);
    pc->forward_log.size = 0;
    log_byte(&pc->forward_log, 0);
  }
}

static void prs_put_control_bit(struct prs_compress_ctx* pc, unsigned char bit) {
  prs_put_control_bit_nosave(pc, bit);
  prs_put_control_save(pc);
}

static void prs_put_static_data(struct prs_compress_ctx* pc, unsigned char data) {
  log_byte(&pc->forward_log, data);
}



static void prs_init(struct prs_compress_ctx* pc, FILE* out) {
  pc->bitpos = 0;
  pc->out = out;
  log_init(&pc->forward_log);
  log_byte(&pc->forward_log, 0);
}

static void prs_finish(struct prs_compress_ctx* pc) {
  prs_put_control_bit(pc, 0);
  prs_put_control_bit(pc, 1);
  if (pc->bitpos != 0)
    pc->forward_log.data[0] = ((pc->forward_log.data[0] << pc->bitpos) >> 8);
  prs_put_static_data(pc, 0);
  prs_put_static_data(pc, 0);
  fwrite(pc->forward_log.data, pc->forward_log.size, 1, pc->out);
  delete_log(&pc->forward_log);
}

static void prs_rawbyte(struct prs_compress_ctx* pc, uint8_t value) {
  prs_put_control_bit_nosave(pc, 1);
  prs_put_static_data(pc, value);
  prs_put_control_save(pc);
}

static void prs_shortcopy(struct prs_compress_ctx* pc, int offset, unsigned char size) {
  size -= 2;
  prs_put_control_bit(pc, 0);
  prs_put_control_bit(pc, 0);
  prs_put_control_bit(pc, (size >> 1) & 1);
  prs_put_control_bit_nosave(pc, size & 1);
  prs_put_static_data(pc, offset & 0xFF);
  prs_put_control_save(pc);
}

static void prs_longcopy(struct prs_compress_ctx* pc, int offset, unsigned char size) {
  if (size <= 9) {
    prs_put_control_bit(pc, 0);
    prs_put_control_bit_nosave(pc, 1);
    prs_put_static_data(pc, ((offset << 3) & 0xF8) | ((size - 2) & 0x07));
    prs_put_static_data(pc, (offset >> 5) & 0xFF);
    prs_put_control_save(pc);
  } else {
    prs_put_control_bit(pc, 0);
    prs_put_control_bit_nosave(pc, 1);
    prs_put_static_data(pc, (offset << 3) & 0xF8);
    prs_put_static_data(pc, (offset >> 5) & 0xFF);
    prs_put_static_data(pc, size - 1);
    prs_put_control_save(pc);
  }
}

static void prs_copy(struct prs_compress_ctx* pc, int offset, unsigned char size) {
  if ((offset > -0x100) && (size <= 5))
    prs_shortcopy(pc, offset, size);
  else
    prs_longcopy(pc, offset, size);
}

int64_t prs_compress_stream(FILE* src, FILE* dst, size_t size) {

  struct data_log backward_log, forward_log;
  struct prs_compress_ctx pc;
  log_init(&backward_log);
  log_init(&forward_log);

  prs_init(&pc, dst);
  while (size > 0 || size == -1) {

    fill_log(&forward_log, src, size);
    if (forward_log.offset == forward_log.size)
      break; // no more data to compress

    int best_offset = 0;
    int best_size = 0;
    int this_offset;
    for (this_offset = -3; (this_offset > -backward_log.size) && (this_offset > -0x1FF0) && (best_size < 255); this_offset--) {
      int this_size = 1;
      while (!memcmp(&backward_log.data[backward_log.size + this_offset],
                     &forward_log.data[forward_log.offset],
                     this_size) &&
             (this_size < 256) &&
             ((this_offset + this_size) < 0) &&
             (this_size <= forward_log.size))
        this_size++;
      this_size--;

      if (this_size > best_size) {
        best_offset = this_offset;
        best_size = this_size;
      }
    }
    if (best_size < 3) {
      prs_rawbyte(&pc, forward_log.data[forward_log.offset]);
      log_byte(&backward_log, forward_log.data[forward_log.offset]);
      forward_log.offset++;
      if (size > 0)
        size--;

    } else {
      prs_copy(&pc, best_offset, best_size);
      log_bytes(&backward_log, &forward_log.data[forward_log.offset + best_offset],
          best_size);
      forward_log.offset += best_size;
      if (size > 0)
        size -= best_size;
    }
  }
  prs_finish(&pc);

  return size;
}


int64_t prs_decompress_stream(FILE* in, FILE* out, int skip_output_bytes,
                           int stop_after_size) {

  struct data_log log;
  log_init(&log);

  int32_t r3, r5;
  int bitpos = 9;
  unsigned char currentbyte;
  int flag;
  int offset;
  unsigned long x, t;
  unsigned long out_size = 0;
  
  currentbyte = fgetc(in);
  for (;;)
  {
    bitpos--;
    if (bitpos == 0)
    {
      currentbyte = fgetc(in);
      bitpos = 8;
    }
    flag = currentbyte & 1;
    currentbyte = currentbyte >> 1;
    if (flag)
    {
      int ch = fgetc(in);
      if (skip_output_bytes)
        skip_output_bytes--;
      else {
        fputc(ch, out);
        out_size++;
        if (stop_after_size && (out_size >= stop_after_size))
          return out_size;
      }
      log_byte(&log, ch);
      continue;
    }
    bitpos--;
    if (bitpos == 0)
    {
      currentbyte = fgetc(in);
      bitpos = 8;
    }
    flag = currentbyte & 1;
    currentbyte = currentbyte >> 1;
    if (flag)
    {
      r3 = fgetc(in) & 0xFF;
      offset = ((fgetc(in) & 0xFF) << 8) | r3;
      if (offset == 0) {
        delete_log(&log);
        return out_size;
      }
      r3 = r3 & 0x00000007;
      r5 = (offset >> 3) | 0xFFFFE000;
      if (r3 == 0)
      {
        flag = 0;
        r3 = fgetc(in) & 0xFF;
        r3++;
      } else r3 += 2;
    } else {
      r3 = 0;
      for (x = 0; x < 2; x++)
      {
        bitpos--;
        if (bitpos == 0)
        {
          currentbyte = fgetc(in);
          bitpos = 8;
        }
        flag = currentbyte & 1;
        currentbyte = currentbyte >> 1;
        offset = r3 << 1;
        r3 = offset | flag;
      }
      offset = fgetc(in) | 0xFFFFFF00;
      r3 += 2;
      r5 = offset;
    }
    if (r3 == 0)
      continue;
    t = r3;
    for (x = 0; x < t; x++)
    {
      int ch = log.data[log.size + r5];
      if (skip_output_bytes)
        skip_output_bytes--;
      else {
        fputc(ch, out);
        out_size++;
        if (stop_after_size && (out_size >= stop_after_size))
          return out_size;
      }
      log_byte(&log, ch);
    }
  }
}
