#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#pragma pack(1)

struct GvmFileEntry {
  uint16_t file_num;
  char name[28];
  uint32_t unknown[2];
};

struct GvmHeader {
  uint32_t magic; // GVMH
  uint16_t unknown[3];
  uint16_t num_files;
  struct GvmFileEntry entries[0];
};

struct GvrHeader {
  uint32_t magic; // GVRT
  uint32_t size;
};

int16_t byteswap16(int16_t a) {
  return ((a >> 8) & 0x00FF) | ((a << 8) & 0xFF00);
}

int32_t byteswap32(int32_t a) {
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
    fprintf(stderr, "failed to open gvm file (error %d)\n", errno);
    return -2;
  }

  struct GvmHeader *gvm = (struct GvmHeader*)malloc(sizeof(struct GvmHeader));
  fread(gvm, sizeof(struct GvmHeader), 1, f);
  gvm->num_files = byteswap16(gvm->num_files);
  if (gvm->magic != 0x484D5647)
    fprintf(stderr, "warning: gvm header may be corrupt\n");

  gvm = (struct GvmHeader*)realloc(gvm, sizeof(struct GvmHeader) + gvm->num_files * sizeof(struct GvmFileEntry));
  fread(&gvm->entries, gvm->num_files, sizeof(struct GvmFileEntry), f);

  int x;
  char filename_buf[0x200];
  for (x = 0; x < gvm->num_files; x++) {
    sprintf(filename_buf, "%s_%s.gvr", argv[1], gvm->entries[x].name);

    // 0x10-align
    fseek(f, (ftell(f) + 0xF) & ~0xF, SEEK_SET);

    struct GvrHeader gvr;
    fread(&gvr, sizeof(struct GvrHeader), 1, f);
    if (gvr.magic != 0x54525647)
      fprintf(stderr, "warning: gvr header may be corrupt\n");
    uint32_t size = gvr.size;

    printf("> %04d: %08X %s\n", x + 1, size, filename_buf);

    FILE* out = fopen(filename_buf, "wb");
    void* data = malloc(size);
    fread(data, size, 1, f);
    fwrite(&gvr, sizeof(struct GvrHeader), 1, out);
    fwrite(data, size, 1, out);
    free(data);
    fclose(out);
  }
  
  fclose(f);

  return 0;
}
