#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "PRSDataLog.hh"


PRSDataLog::PRSDataLog()
  : data(0x8000, '\0'),
    size(0),
    offset(0) { }

void PRSDataLog::add(uint8_t v) {
  if (this->size == 0x8000) {
    memmove(this->data.data(), this->data.data() + 0x4000, 0x4000);
    this->size -= 0x4000;
    this->offset -= 0x4000;
  }
  this->data[this->size] = v;
  this->size++;
}

void PRSDataLog::add(const void* data, size_t size) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  for (; size > 0; bytes++, size--) {
    this->add(*bytes);
  }
}

void PRSDataLog::fill(FILE* f, size_t max_read) {
  if (this->offset > 0x6000) {
    this->offset -= 0x2000;
    this->size -= 0x2000;
    memmove(this->data.data(), this->data.data() + 0x2000, this->size);
  }
  if (this->size < 0x8000) {
    size_t bytes_to_read = 0x8000 - this->size;
    if (bytes_to_read > max_read) {
      bytes_to_read = max_read;
    }
    if (bytes_to_read) {
      this->size += fread(
          this->data.data() + (0x8000 - bytes_to_read), 1, bytes_to_read, f);
    }
  }
}
