#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#pragma pack(1)

typedef struct {
  char name[0x20];
  int32_t unknown;
  int32_t size;
  int32_t unknown2[2];
} GslEntry;

int32_t byteswap(int32_t a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00) |
         ((a << 8) & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

int main(int argc, char* argv[]) {

  if (argc != 2) {
    fprintf(stderr, "usage: %s <filename>\n", argv[0]);
    return -1;
  }

  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "failed to open gsl file (error %d)\n", errno);
    return -2;
  }

  GslEntry entries[0x100];
  fread(&entries, sizeof(GslEntry), 0x100, f);

  int x;
  for (x = 0; x < 0x100; x++) {
    if (!entries[x].name[0]) {
      continue;
    }

    entries[x].size = byteswap(entries[x].size);
    printf("> %08X %s\n", entries[x].size, entries[x].name);

    int offset = ftell(f);
    offset = (offset + 0x7FF) & ~0x7FF;
    fseek(f, offset, SEEK_SET);

    FILE* out = fopen(entries[x].name, "wb");
    void* data = malloc(entries[x].size);
    fread(data, entries[x].size, 1, f);
    fwrite(data, entries[x].size, 1, out);
    free(data);
    fclose(out);
  }

  fclose(f);
  return 0;
}
