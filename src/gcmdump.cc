#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <string>
#include <unordered_set>

using namespace std;

struct ApploaderHeader {
  char date[0x10];
  phosg::be_uint32_t entrypoint;
  phosg::be_uint32_t size;
  phosg::be_uint32_t trailer_size;
  phosg::be_uint32_t unknown_a1;
  // Apploader code follows immediately (loaded to 0x81200000)
} __attribute__((packed));

struct GCMHeader {
  phosg::be_uint32_t game_id;
  phosg::be_uint16_t company_id;
  uint8_t disk_id;
  uint8_t version;
  uint8_t audio_streaming;
  uint8_t stream_buffer_size;
  uint8_t unused1[0x0E];
  phosg::be_uint32_t wii_magic;
  phosg::be_uint32_t gc_magic;
  char name[0x03E0];
  phosg::be_uint32_t debug_offset;
  phosg::be_uint32_t debug_addr;
  uint8_t unused2[0x18];
  phosg::be_uint32_t dol_offset;
  phosg::be_uint32_t fst_offset;
  phosg::be_uint32_t fst_size;
  phosg::be_uint32_t fst_max_size;
} __attribute__((packed));

struct TGCHeader {
  phosg::be_uint32_t magic;
  phosg::be_uint32_t unknown1;
  phosg::be_uint32_t header_size;
  phosg::be_uint32_t unknown2;
  phosg::be_uint32_t fst_offset;
  phosg::be_uint32_t fst_size;
  phosg::be_uint32_t fst_max_size;
  phosg::be_uint32_t dol_offset;
  phosg::be_uint32_t dol_size;
  phosg::be_uint32_t file_area;
  phosg::be_uint32_t file_area_size;
  phosg::be_uint32_t banner_offset;
  phosg::be_uint32_t banner_size;
  phosg::be_uint32_t file_offset_base;
} __attribute__((packed));

union ImageHeader {
  GCMHeader gcm;
  TGCHeader tgc;
} __attribute__((packed));

struct DOLHeader {
  // Sections 0-6 are text; the rest (7-17) are data
  phosg::be_uint32_t section_offset[18];
  phosg::be_uint32_t section_address[18];
  phosg::be_uint32_t section_size[18];
  phosg::be_uint32_t bss_address;
  phosg::be_uint32_t bss_size;
  phosg::be_uint32_t entry_point;
  phosg::be_uint32_t unused[7];
} __attribute__((packed));

struct FSTRootEntry {
  phosg::be_uint32_t dir_flag_string_offset;
  phosg::be_uint32_t parent_offset;
  phosg::be_uint32_t num_entries;
} __attribute__((packed));

struct FSTDirEntry {
  phosg::be_uint32_t dir_flag_string_offset;
  phosg::be_uint32_t parent_offset;
  phosg::be_uint32_t next_offset;
} __attribute__((packed));

struct FSTFileEntry {
  phosg::be_uint32_t dir_flag_string_offset;
  phosg::be_uint32_t file_offset;
  phosg::be_uint32_t file_size;
} __attribute__((packed));

union FSTEntry {
  FSTRootEntry root;
  FSTDirEntry dir;
  FSTFileEntry file;

  bool is_dir() const {
    return this->file.dir_flag_string_offset & 0xFF000000;
  }
  uint32_t string_offset() const {
    return this->file.dir_flag_string_offset & 0x00FFFFFF;
  }
} __attribute__((packed));

static string sanitize_filename(const string& name) {
  string ret = name;
  for (auto& ch : ret) {
    if (ch < 0x20 || ch > 0x7E) {
      ch = '_';
    }
  }
  return ret;
}

uint32_t dol_file_size(const DOLHeader* dol) {
  static const int num_sections = 18;
  uint32_t x, max_offset = 0;
  for (x = 0; x < num_sections; x++) {
    uint32_t section_end_offset = dol->section_offset[x] + dol->section_size[x];
    if (section_end_offset > max_offset) {
      max_offset = section_end_offset;
    }
  }
  return max_offset;
}

void parse_until(
    phosg::scoped_fd& fd,
    const FSTEntry* fst,
    const char* string_table,
    int start,
    int end,
    int64_t base_offset,
    const unordered_set<string>& target_filenames) {

  int x;
  string pwd = std::filesystem::current_path().string();
  pwd += '/';
  size_t pwd_end = pwd.size();
  for (x = start; x < end; x++) {
    if (fst[x].is_dir()) {
      fprintf(stderr, "> entry: %08X $ %08X %08X %08X %s%s/\n", x,
          fst[x].file.dir_flag_string_offset.load(),
          fst[x].file.file_offset.load(),
          fst[x].file.file_size.load(), pwd.c_str(),
          &string_table[fst[x].string_offset()]);

      pwd += sanitize_filename(&string_table[fst[x].file.dir_flag_string_offset & 0x00FFFFFF]);
      if (mkdir(pwd.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) &&
          (errno != EEXIST)) {
        throw runtime_error("cannot create directory " + pwd);
      }
      if (chdir(pwd.c_str())) {
        throw runtime_error("cannot enter directory " + pwd);
      }
      parse_until(fd, fst, string_table, x + 1, fst[x].dir.next_offset,
          base_offset, target_filenames);
      pwd.resize(pwd_end);
      if (chdir(pwd.c_str())) {
        throw runtime_error("cannot return to directory " + pwd);
      }

      x = fst[x].dir.next_offset - 1;

    } else {
      fprintf(stderr, "> entry: %08X $ %08X %08X %08X %s%s\n", x,
          fst[x].file.dir_flag_string_offset.load(),
          fst[x].file.file_offset.load(), fst[x].file.file_size.load(),
          pwd.c_str(), &string_table[fst[x].string_offset()]);

      if (target_filenames.empty() ||
          target_filenames.count(&string_table[fst[x].string_offset()])) {
        string filename = sanitize_filename(&string_table[fst[x].string_offset()]);
        try {
          phosg::save_file(filename, preadx(fd, fst[x].file.file_size, fst[x].file.file_offset + base_offset));
        } catch (const exception& e) {
          fprintf(stderr, "!!! failed to write file: %s\n", e.what());
        }
      }
    }
  }
}

enum Format {
  UNKNOWN = 0,
  GCM = 1,
  TGC = 2,
};

int main(int argc, char* argv[]) {

  if (argc < 2) {
    fprintf(stderr, "Usage: %s [--gcm|--tgc] <filename> [files_to_extract]\n", argv[0]);
    return -1;
  }

  Format format = Format::UNKNOWN;
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

  phosg::scoped_fd fd(filename, O_RDONLY);

  ImageHeader header;
  phosg::readx(fd, &header, sizeof(ImageHeader));
  if (format == Format::UNKNOWN) {
    if (header.gcm.gc_magic == 0xC2339F3D) {
      format = Format::GCM;
    } else if (header.tgc.magic == 0xAE0F38A2) {
      format = Format::TGC;
    } else {
      fprintf(stderr, "can\'t determine archive type of %s\n", filename);
      return -3;
    }
  }

  uint32_t gcm_offset, fst_offset, fst_size, dol_offset;
  int32_t base_offset;
  if (format == Format::GCM) {
    fprintf(stderr, "format: gcm (%s)\n", header.gcm.name);
    gcm_offset = 0;
    fst_offset = header.gcm.fst_offset;
    fst_size = header.gcm.fst_size;
    base_offset = 0;
    dol_offset = header.gcm.dol_offset;

  } else if (format == Format::TGC) {
    fprintf(stderr, "format: tgc\n");
    gcm_offset = header.tgc.header_size;
    fst_offset = header.tgc.fst_offset;
    fst_size = header.tgc.fst_size;
    base_offset = header.tgc.file_area - header.tgc.file_offset_base;
    dol_offset = header.tgc.dol_offset;

  } else {
    fprintf(stderr, "can\'t determine format; use one of --tgc or --gcm\n");
    return -3;
  }

  // if there are target filenames and default.dol isn't specified, don't
  // extract it
  if (target_filenames.empty() || target_filenames.count("default.dol")) {
    string dol_data = phosg::preadx(fd, sizeof(DOLHeader), dol_offset);
    uint32_t dol_size = dol_file_size(reinterpret_cast<const DOLHeader*>(
        dol_data.data()));

    dol_data += phosg::preadx(fd, dol_size - sizeof(DOLHeader),
        dol_offset + sizeof(DOLHeader));

    phosg::save_file("default.dol", dol_data);
  }

  if (target_filenames.empty() || target_filenames.count("__gcm_header__.bin")) {
    phosg::save_file("__gcm_header__.bin", preadx(fd, 0x2440, gcm_offset));
  }

  if (target_filenames.empty() || target_filenames.count("apploader.bin")) {
    string data = phosg::preadx(fd, sizeof(ApploaderHeader), gcm_offset + 0x2440);
    const auto* header = reinterpret_cast<const ApploaderHeader*>(data.data());
    data += phosg::preadx(
        fd, header->size + header->trailer_size, gcm_offset + 0x2440 + sizeof(ApploaderHeader));
    phosg::save_file("apploader.bin", data);
  }

  string fst_data = phosg::preadx(fd, fst_size, fst_offset);
  const FSTEntry* fst = reinterpret_cast<const FSTEntry*>(fst_data.data());

  // if there are target filenames and fst.bin isn't specified, don't extract it
  if (target_filenames.empty() || target_filenames.count("fst.bin")) {
    phosg::save_file("fst.bin", fst_data);
  }

  int num_entries = fst[0].root.num_entries;
  fprintf(stderr, "> root: %08X files\n", num_entries);

  char* string_table = (char*)fst + (sizeof(FSTEntry) * num_entries);
  parse_until(fd, fst, string_table, 1, num_entries, base_offset,
      target_filenames);

  return 0;
}
