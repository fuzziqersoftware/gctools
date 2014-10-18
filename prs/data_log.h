#include <stdint.h>


struct data_log {
  uint8_t* data;
  int size;
  int offset;
};


void log_init(struct data_log* log);
void delete_log(struct data_log* log);
void log_byte(struct data_log* log, int ch);
void log_bytes(struct data_log* log, void* data, size_t size);
void fill_log(struct data_log* log, FILE* f, size_t max_read);
