#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "prs/prs.h"

typedef struct {
  int32_t unknown1;
  int32_t decompressed_size;
  int32_t unknown2[5];
  int32_t gvmoffset;
} PaeHeader;

int32_t byteswap(int32_t a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00) |
  ((a << 8) & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

int main(int argc, char* argv[]) {

  if (argc != 2) {
    fprintf(stderr, "usage: %s <filename>\n", argv[0]);
    return 1;
  }

  FILE* pae = fopen(argv[1], "rb");
  if (!pae) {
    fprintf(stderr, "failed to open pae file (error %d)\n", errno);
    return 2;
  }

  PaeHeader header;
  fread(&header, sizeof(PaeHeader), 1, pae);

  char out_filename[0x200];
  snprintf(out_filename, 0x200, "%s.dec", argv[1]);
  FILE* dec = fopen(out_filename, "w+b");
  if (!dec) {
    fprintf(stderr, "failed to open dec file (error %d)\n", errno);
    return 2;
  }

  fwrite(&header, sizeof(PaeHeader), 1, dec);
  prs_decompress_stream(pae, dec, 0);
  fclose(pae);

  snprintf(out_filename, 0x200, "%s.gvm", argv[1]);
  FILE* gvm = fopen(out_filename, "wb");
  if (!gvm) {
    fprintf(stderr, "failed to open gvm file (error %d)\n", errno);
    return 2;
  }

  uint32_t size = byteswap(header.decompressed_size);
  fseek(dec, byteswap(header.gvmoffset) + sizeof(PaeHeader), SEEK_SET);
  void* data = malloc(size);
  if (!data) {
    fprintf(stderr, "failed to allocate memory for gvm copy\n");
    return 3;
  }

  fread(data, size, 1, dec);
  fwrite(data, size, 1, gvm);

  free(data);
  fclose(dec);
  fclose(gvm);

  return 0;
}
