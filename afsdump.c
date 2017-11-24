#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#pragma pack(1)

typedef struct {
  int32_t offset;
  int32_t size;
} FileEntry;

typedef struct {
  int32_t magic;
  int32_t numfiles;
  FileEntry entries[0];
} AfsHeader;

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
    fprintf(stderr, "failed to open afs file (error %d)\n", errno);
    return -2;
  }

  AfsHeader *afs = (AfsHeader*)malloc(sizeof(AfsHeader));
  fread(afs, sizeof(AfsHeader), 1, f);
  if (afs->magic != 0x00534641) {
    fprintf(stderr, "warning: afs header may be corrupt\n");
  }
  afs = (AfsHeader*)realloc(afs, sizeof(AfsHeader) + afs->numfiles * sizeof(FileEntry));

  fread(afs->entries, afs->numfiles, sizeof(FileEntry), f);

  int32_t filename_offset;
  fseek(f, 0x7FFF8, SEEK_SET);
  fread(&filename_offset, sizeof(int32_t), 1, f);

  int x;
  char filename_buf[0x30];
  for (x = 0; x < afs->numfiles; x++) {
    if (filename_offset) {
      fseek(f, filename_offset + 0x30 * x, SEEK_SET);
      fread(filename_buf, 0x30, 1, f);
    } else {
      if (argc > 2) {
        sprintf(filename_buf, "%s_%d%s", argv[1], x, argv[2]);
      } else {
        sprintf(filename_buf, "%s_%d", argv[1], x);
      }
    }

    printf("> %04d: %08X %08X %s\n", x + 1, afs->entries[x].offset, afs->entries[x].size, filename_buf);

    FILE* out = fopen(filename_buf, "wb");
    void* data = malloc(afs->entries[0].size);
    fseek(f, afs->entries[x].offset, SEEK_SET);
    fread(data, afs->entries[x].size, 1, f);
    fwrite(data, afs->entries[x].size, 1, out);
    free(data);
    fclose(out);
  }
  
  fclose(f);

  return 0;
}
