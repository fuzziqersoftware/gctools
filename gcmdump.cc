#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <string>
#include <unordered_set>

using namespace std;


#pragma pack(1)

struct GCMHeader {
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
};

struct TGCHeader {
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
};

union ImageHeader {
  GCMHeader gcm;
  TGCHeader tgc;
};

struct DOLHeader {
  uint32_t text_offset[7];
  uint32_t data_offset[11];
  uint32_t text_address[7];
  uint32_t data_address[11];
  uint32_t text_size[7];
  uint32_t data_size[11];
  uint32_t bss_address;
  uint32_t bss_size;
  uint32_t entry_point;
  uint32_t unused[7];
};

struct FSTRootEntry {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t parent_offset;
  int32_t num_entries;
};

struct FSTDirEntry {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t parent_offset;
  int32_t next_offset;
};

struct FSTFileEntry {
  int8_t dir_flag;
  int32_t string_offset:24;
  int32_t file_offset;
  int32_t file_size;
};

union FSTEntry {
  FSTRootEntry root;
  FSTDirEntry dir;
  FSTFileEntry file;
};


int32_t byteswap(int32_t a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00) |
         ((a << 8) & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

uint32_t dol_file_size(DOLHeader* dol) {
  static const int num_sections = 18;
  uint32_t x, max_offset = 0;
  for (x = 0; x < num_sections; x++) {
    uint32_t section_end_offset = byteswap(dol->text_offset[x]) +
        byteswap(dol->text_size[x]);
    if (section_end_offset > max_offset)
      max_offset = section_end_offset;
  }
  return max_offset;
}

void parse_until(FILE* f, FSTEntry* fst, const char* string_table, int start,
    int end, int64_t base_offset, const unordered_set<string>& target_filenames) {

  int x;
  char pwd[0x100];
  getcwd(pwd, 0x100);
  strcat(pwd, "/");
  int pwd_end = strlen(pwd);
  for (x = start; x < end; x++) {
    FSTEntry this_entry;
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
          base_offset, target_filenames);
      pwd[pwd_end] = 0;
      chdir(pwd);

      x = this_entry.dir.next_offset - 1;

    } else {
      printf("> entry: %08X $ %02X %08X %08X %08X %s%s\n", x,
             this_entry.file.dir_flag, this_entry.file.string_offset,
             this_entry.file.file_offset, this_entry.file.file_size, pwd,
             &string_table[this_entry.file.string_offset]);

      if (target_filenames.empty() ||
          target_filenames.count(&string_table[this_entry.file.string_offset])) {
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
}


enum Format {
  Unknown = 0,
  GCM = 1,
  TGC = 2,
};

int main(int argc, char* argv[]) {

  if (argc < 2) {
    fprintf(stderr, "usage: %s [--gcm|--tgc] <filename> [files_to_extract]\n", argv[0]);
    return -1;
  }

  Format format = Format::Unknown;
  const char* filename = NULL;
  unordered_set<string> target_filenames;
  for (int x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "--gcm")) {
      format = Format::GCM;
    } else if (!strcmp(argv[x], "--tgc")) {
      format = Format::TGC;
    } else if (!filename) {
      filename = argv[x];
    } else {
      target_filenames.emplace(argv[x]);
    }
  }
  if (!filename) {
    fprintf(stderr, "no filename given\n");
    return -1;
  }

  FILE* f = fopen(filename, "rb");
  if (!f) {
    fprintf(stderr, "failed to open %s (error %d)\n", filename, errno);
    return -2;
  }

  ImageHeader header;
  fread(&header, sizeof(ImageHeader), 1, f);
  if (format == Format::Unknown) {
    if (header.gcm.gc_magic == 0x3D9F33C2) {
      format = Format::GCM;
    } else if (header.tgc.magic == 0xA2380FAE) {
      format = Format::TGC;
    } else {
      fprintf(stderr, "can\'t determine archive type of %s\n", filename);
      return -3;
    }
  }

  uint32_t fst_offset, fst_size, dol_offset;
  int32_t base_offset;
  if (format == Format::GCM) {
    printf("format: gcm (%s)\n", header.gcm.name);
    fst_offset = byteswap(header.gcm.fst_offset);
    fst_size = byteswap(header.gcm.fst_size);
    base_offset = 0;
    dol_offset = byteswap(header.gcm.dol_offset);

  } else if (format == Format::TGC) {
    printf("format: tgc\n");
    fst_offset = byteswap(header.tgc.fst_offset);
    fst_size = byteswap(header.tgc.fst_size);
    base_offset = byteswap(header.tgc.file_area) - byteswap(header.tgc.file_offset_base);
    dol_offset = byteswap(header.tgc.dol_offset);

  } else {
    fprintf(stderr, "can\'t determine format; use one of --tgc or --gcm\n");
    return -3;
  }

  // if there are target filenames and default.dol isn't specified, don't
  // extract it
  if (target_filenames.empty() || target_filenames.count("default.dol")) {
    fseek(f, dol_offset, SEEK_SET);
    DOLHeader dol;
    fread(&dol, sizeof(DOLHeader), 1, f);
    uint32_t dol_size = dol_file_size(&dol) - sizeof(DOLHeader);

    void* dol_data = malloc(dol_size);
    fread(dol_data, dol_size, 1, f);

    FILE* dol_file = fopen("default.dol", "wb");
    fwrite(&dol, sizeof(DOLHeader), 1, dol_file);
    fwrite(dol_data, dol_size, 1, dol_file);

    fclose(dol_file);
    free(dol_data);
  }

  fseek(f, fst_offset, SEEK_SET);

  FSTEntry* fst = (FSTEntry*)malloc(fst_size);
  fread(fst, fst_size, 1, f);

  int num_entries = byteswap(fst[0].root.num_entries);
  printf("> root: %08X files\n", num_entries);

  char* string_table = (char*)fst + (sizeof(FSTEntry) * num_entries);
  parse_until(f, fst, string_table, 1, num_entries, base_offset,
      target_filenames);

  fclose(f);

  return 0;
}
