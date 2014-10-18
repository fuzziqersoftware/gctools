#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

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
    return -1;
  }

  char cmd[0x80];
  sprintf(cmd, "prs -d --raw-bytes=0x20 < %s > %s.dec", argv[1], argv[1]);
  int retcode = system(cmd);
  if (retcode) {
    fprintf(stderr, "error: \"%s\" failed with error %d\n", cmd, retcode);
    return retcode;
  }

  char filename_in[0x30];
  sprintf(filename_in, "%s.dec", argv[1]);
  FILE* f = fopen(filename_in, "rb");
  if (!f) {
    fprintf(stderr, "failed to open pae file (error %d)\n", errno);
    return -2;
  }

  PaeHeader pae;
  fread(&pae, sizeof(PaeHeader), 1, f);
  pae.decompressed_size = byteswap(pae.decompressed_size);
  pae.gvmoffset = byteswap(pae.gvmoffset);

  void* data = malloc(pae.decompressed_size);
  fread(data, pae.decompressed_size, 1, f);

  char filename_out[0x30];
  sprintf(filename_out, "%s.gvm", argv[1]);

  FILE* out = fopen(filename_out, "wb");
  fwrite(((char*)data + pae.gvmoffset), pae.decompressed_size - pae.gvmoffset, 1, out);
  free(data);
  fclose(out);
  fclose(f);

  return 0;
}
