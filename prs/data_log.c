#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_log.h"
#include "util.h"


void log_init(struct data_log* log) {
  log->data = malloc(0x8000);
  log->size = 0;
  log->offset = 0;
}

void delete_log(struct data_log* log) {
  if (log->data) {
    free(log->data);
  }
}

void log_byte(struct data_log* log, int ch) {
  if (log->size == 0x8000) {
    memmove(log->data, &log->data[0x4000], 0x4000);
    log->size -= 0x4000;
    log->offset -= 0x4000;
  }
  log->data[log->size] = ch;
  log->size++;
}

void log_bytes(struct data_log* log, void* data, size_t size) {
  uint8_t* bytes = (uint8_t*)data;
  for (; size > 0; bytes++, size--) {
    log_byte(log, *bytes);
  }
}

void fill_log(struct data_log* log, FILE* f, size_t max_read) {
  if (log->offset > 0x6000) {
    log->offset -= 0x2000;
    log->size -= 0x2000;
    memmove(log->data, &log->data[0x2000], log->size);
  }
  if (log->size < 0x8000) {
    int bytes_to_read = 0x8000 - log->size;
    if (bytes_to_read > max_read) {
      bytes_to_read = max_read;
    }
    if (bytes_to_read) {
      log->size += fread(&log->data[0x8000 - bytes_to_read], 1, bytes_to_read, f);
    }
  }
}
