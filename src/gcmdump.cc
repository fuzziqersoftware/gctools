#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <string>
#include <unordered_set>

using namespace std;


#pragma pack(1)

struct GCMHeader {
  uint32_t game_id;
  uint16_t company_id;
  uint8_t disk_id;
  uint8_t version;
  uint8_t audio_streaming;
  uint8_t stream_buffer_size;
  uint8_t unused1[0x0E];
  uint32_t wii_magic;
  uint32_t gc_magic;
  char name[0x03E0];
  uint32_t debug_offset;
  uint32_t debug_addr;
  uint8_t unused2[0x18];
  uint32_t dol_offset;
  uint32_t fst_offset;
  uint32_t fst_size;
  uint32_t fst_max_size;
};

struct TGCHeader {
  uint32_t magic;
  uint32_t unknown1;
  uint32_t header_size;
  uint32_t unknown2;
  uint32_t fst_offset;
  uint32_t fst_size;
  uint32_t fst_max_size;
  uint32_t dol_offset;
  uint32_t dol_size;
  uint32_t file_area;
  uint32_t unknown3; // 10
  uint32_t banner_offset;
  uint32_t banner_size;
  uint32_t file_offset_base;
};

union ImageHeader {
  GCMHeader gcm;
  TGCHeader tgc;
};

struct DOLHeader {
  // Sections 0-6 are text; the rest (7-17) are data
  uint32_t section_offset[18];
  uint32_t section_address[18];
  uint32_t section_size[18];
  uint32_t bss_address;
  uint32_t bss_size;
  uint32_t entry_point;
  uint32_t unused[7];
};

struct FSTRootEntry {
  uint8_t dir_flag;
  uint32_t string_offset:24;
  uint32_t parent_offset;
  uint32_t num_entries;
};

struct FSTDirEntry {
  uint8_t dir_flag;
  uint32_t string_offset:24;
  uint32_t parent_offset;
  uint32_t next_offset;
};

struct FSTFileEntry {
  uint8_t dir_flag;
  uint32_t string_offset:24;
  uint32_t file_offset;
  uint32_t file_size;
};

union FSTEntry {
  FSTRootEntry root;
  FSTDirEntry dir;
  FSTFileEntry file;
};


uint32_t dol_file_size(const DOLHeader* dol) {
  static const int num_sections = 18;
  uint32_t x, max_offset = 0;
  for (x = 0; x < num_sections; x++) {
    uint32_t section_end_offset = bswap32(dol->section_offset[x]) +
        bswap32(dol->section_size[x]);
    if (section_end_offset > max_offset) {
      max_offset = section_end_offset;
    }
  }
  return max_offset;
}

void parse_until(scoped_fd& fd, const FSTEntry* fst, const char* string_table,
    int start, int end, int64_t base_offset,
    const unordered_set<string>& target_filenames) {

  int x;
  string pwd = getcwd();
  pwd += '/';
  size_t pwd_end = pwd.size();
  for (x = start; x < end; x++) {
    FSTEntry this_entry;
    this_entry.file.dir_flag = fst[x].file.dir_flag;
    this_entry.file.string_offset = bswap32(fst[x].file.string_offset) >> 8;
    this_entry.file.file_offset = bswap32(fst[x].file.file_offset);
    this_entry.file.file_size = bswap32(fst[x].file.file_size);

    if (this_entry.file.dir_flag) {
      fprintf(stderr, "> entry: %08X $ %02X %08X %08X %08X %s%s/\n", x,
             this_entry.file.dir_flag, this_entry.file.string_offset,
             this_entry.file.file_offset, this_entry.file.file_size, pwd.c_str(),
             &string_table[this_entry.file.string_offset]);

      pwd += &string_table[this_entry.file.string_offset];
      if (mkdir(pwd.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
        throw runtime_error("cannot create directory " + pwd);
      }
      if (chdir(pwd.c_str())) {
        throw runtime_error("cannot enter directory " + pwd);
      }
      parse_until(fd, fst, string_table, x + 1, this_entry.dir.next_offset,
          base_offset, target_filenames);
      pwd.resize(pwd_end);
      if (chdir(pwd.c_str())) {
        throw runtime_error("cannot return to directory " + pwd);
      }

      x = this_entry.dir.next_offset - 1;

    } else {
      fprintf(stderr, "> entry: %08X $ %02X %08X %08X %08X %s%s\n", x,
             this_entry.file.dir_flag, this_entry.file.string_offset,
             this_entry.file.file_offset, this_entry.file.file_size, pwd.c_str(),
             &string_table[this_entry.file.string_offset]);

      if (target_filenames.empty() ||
          target_filenames.count(&string_table[this_entry.file.string_offset])) {
        // some games have non-ascii chars in filenames; get rid of them
        string filename(&string_table[this_entry.file.string_offset]);
        for (auto& ch : filename) {
          if (ch < 0x20 || ch > 0x7E) {
            ch = '_';
          }
        }

        save_file(filename, preadx(fd, this_entry.file.file_size,
            this_entry.file.file_offset + base_offset));
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
    fprintf(stderr, "Usage: %s [--gcm|--tgc] <filename> [files_to_extract]\n", argv[0]);
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

  scoped_fd fd(filename, O_RDONLY);

  ImageHeader header;
  readx(fd, &header, sizeof(ImageHeader));
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
    fprintf(stderr, "format: gcm (%s)\n", header.gcm.name);
    fst_offset = bswap32(header.gcm.fst_offset);
    fst_size = bswap32(header.gcm.fst_size);
    base_offset = 0;
    dol_offset = bswap32(header.gcm.dol_offset);

  } else if (format == Format::TGC) {
    fprintf(stderr, "format: tgc\n");
    fst_offset = bswap32(header.tgc.fst_offset);
    fst_size = bswap32(header.tgc.fst_size);
    base_offset = bswap32(header.tgc.file_area) - bswap32(header.tgc.file_offset_base);
    dol_offset = bswap32(header.tgc.dol_offset);

  } else {
    fprintf(stderr, "can\'t determine format; use one of --tgc or --gcm\n");
    return -3;
  }

  // if there are target filenames and default.dol isn't specified, don't
  // extract it
  if (target_filenames.empty() || target_filenames.count("default.dol")) {
    string dol_data = preadx(fd, sizeof(DOLHeader), dol_offset);
    uint32_t dol_size = dol_file_size(reinterpret_cast<const DOLHeader*>(
        dol_data.data()));

    dol_data += preadx(fd, dol_size - sizeof(DOLHeader),
        dol_offset + sizeof(DOLHeader));

    save_file("default.dol", dol_data);
  }

  string fst_data = preadx(fd, fst_size, fst_offset);
  const FSTEntry* fst = reinterpret_cast<const FSTEntry*>(fst_data.data());

  // if there are target filenames and fst.bin isn't specified, don't extract it
  if (target_filenames.empty() || target_filenames.count("fst.bin")) {
    save_file("fst.bin", fst_data);
  }

  int num_entries = bswap32(fst[0].root.num_entries);
  fprintf(stderr, "> root: %08X files\n", num_entries);

  char* string_table = (char*)fst + (sizeof(FSTEntry) * num_entries);
  parse_until(fd, fst, string_table, 1, num_entries, base_offset,
      target_filenames);

  return 0;
}
