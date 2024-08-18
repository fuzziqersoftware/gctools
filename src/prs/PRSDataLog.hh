#include <stdint.h>
#include <stdio.h>

#include <string>

struct PRSDataLog {
  std::string data;
  int size;
  int offset;

  PRSDataLog();
  void add(uint8_t ch);
  void add(const void* data, size_t size);
  size_t fill(FILE* f, size_t max_read);
};
