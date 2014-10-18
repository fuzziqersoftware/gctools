#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#pragma pack(1)

typedef struct {
  int32_t game_id;
  int16_t company_id;
  int8_t disk_id;
  int8_t version;
  int8_t audio_streaming;
  int8_t stream_buffer_size;
  int8_t unused1[0x0E];
  int32_t wii_magic;
  int32_t gc_magic;
  char name[0x03E0];
  int32_t debug_offset;
  int32_t debug_addr;
  int8_t unused2[0x18];
  int32_t dol_offset;
  int32_t fst_offset;
  int32_t fst_size;
  int32_t fst_max_size;
} GcmHeader;

typedef struct {
  int32_t magic;
  int32_t unknown1;
  int32_t header_size;
  int32_t unknown2;
  int32_t fst_offset;
  int32_t fst_size;
  int32_t fst_max_size;
  int32_t dol_offset;
  int32_t dol_size;
  int32_t file_area;
  int32_t unknown3; // 10
  int32_t banner_offset;
  int32_t banner_size;
  int32_t file_offset_base;
} TgcHeader;

typedef union {
  GcmHeader gcm;
  TgcHeader tgc;
} ImageHeader;

typedef struct {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t parent_offset;
  int32_t num_entries;
} FstRootEntry;

typedef struct {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t parent_offset;
  int32_t next_offset;
} FstDirEntry;

typedef struct {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t file_offset;
  int32_t file_size;
} FstFileEntry;

typedef union {
  FstRootEntry root;
  FstDirEntry dir;
  FstFileEntry file;
} FstEntry;

int32_t byteswap(int32_t a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00) |
         ((a << 8) & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

void parse_until(FILE* f, FstEntry* fst, const char* string_table, int start,
    int end, int64_t base_offset, char* print_file) {

  int x;
  char pwd[0x100];
  getcwd(pwd, 0x100);
  strcat(pwd, "/");
  int pwd_end = strlen(pwd);
  for (x = start; x < end; x++) {
    FstEntry this_entry;
    this_entry.file.dir_flag = fst[x].file.dir_flag;
    this_entry.file.string_offset = byteswap(fst[x].file.string_offset) >> 8;
    this_entry.file.file_offset = byteswap(fst[x].file.file_offset);
    this_entry.file.file_size = byteswap(fst[x].file.file_size);

    if (this_entry.file.dir_flag) {
      printf("> entry: %08X $ %02X %08X %08X %08X %s%s/\n", x,
             this_entry.file.dir_flag, this_entry.file.string_offset,
             this_entry.file.file_offset, this_entry.file.file_size, pwd,
             &string_table[this_entry.file.string_offset]);

      strcpy(&pwd[pwd_end], &string_table[this_entry.file.string_offset]);
      mkdir(pwd, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
      chdir(pwd);
      parse_until(f, fst, string_table, x + 1, this_entry.dir.next_offset,
          base_offset, print_file);
      pwd[pwd_end] = 0;
      chdir(pwd);

      x = this_entry.dir.next_offset - 1;

    } else {
      printf("> entry: %08X $ %02X %08X %08X %08X %s%s\n", x,
             this_entry.file.dir_flag, this_entry.file.string_offset,
             this_entry.file.file_offset, this_entry.file.file_size, pwd,
             &string_table[this_entry.file.string_offset]);

      void* data = malloc(this_entry.file.file_size);
      int data_offset = this_entry.file.file_offset + base_offset;
      fseek(f, data_offset, SEEK_SET);
      fread(data, this_entry.file.file_size, 1, f);

      FILE* out = fopen(&string_table[this_entry.file.string_offset], "wb");
      fwrite(data, this_entry.file.file_size, 1, out);
      fclose(out);

      free(data);
    }
  }
}


#define FORMAT_UNKNOWN  0
#define FORMAT_GCM      1
#define FORMAT_TGC      2

int main(int argc, char* argv[]) {

  if (argc < 2) {
    fprintf(stderr, "usage: %s [--gcm|--tgc] <filename>\n", argv[0]);
    return -1;
  }

  int x;
  int format = FORMAT_UNKNOWN;
  const char* filename = NULL;
  for (x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "--gcm"))
      format = FORMAT_GCM;
    else if (!strcmp(argv[x], "--tgc"))
      format = FORMAT_TGC;
    else
      filename = argv[x];
  }
  if (!filename) {
    fprintf(stderr, "no filename given\n");
    return -1;
  }

  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "failed to open file (error %d)\n", errno);
    return -2;
  }

  ImageHeader header;
  fread(&header, sizeof(ImageHeader), 1, f);
  if (format == FORMAT_UNKNOWN) {
    if (header.gcm.gc_magic == 0x3D9F33C2)
      format = FORMAT_GCM;
    else if (header.tgc.magic == 0xA2380FAE)
      format = FORMAT_TGC;
  }

  uint32_t fst_offset, fst_size;
  int32_t base_offset;
  if (format == FORMAT_GCM) {
    printf("format: gcm (%s)\n", header.gcm.name);
    fst_offset = byteswap(header.gcm.fst_offset);
    fst_size = byteswap(header.gcm.fst_size);
    base_offset = 0;

  } else if (format == FORMAT_TGC) {
    printf("format: tgc\n");
    fst_offset = byteswap(header.tgc.fst_offset);
    fst_size = byteswap(header.tgc.fst_size);
    base_offset = byteswap(header.tgc.file_area) - byteswap(header.tgc.file_offset_base);

  } else {
    fprintf(stderr, "can\'t figure out format; use one of --tgc or --gcm\n");
    return -3;
  }

  fseek(f, fst_offset, SEEK_SET);

  FstEntry* fst = (FstEntry*)malloc(fst_size);
  fread(fst, fst_size, 1, f);

  int num_entries = byteswap(fst[0].root.num_entries);
  printf("> root: %08X files\n", num_entries);

  char* string_table = (char*)fst + (sizeof(FstEntry) * num_entries);
  parse_until(f, fst, string_table, 1, num_entries, base_offset,
      argc > 2 ? argv[2] : NULL);

  fclose(f);

  return 0;
}
