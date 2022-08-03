#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <array>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

using namespace std;

template <typename U32T>
struct GSLEntry {
  char name[0x20];
  be_int32_t unknown;
  U32T size;
  be_int32_t unknown2[2];
} __attribute__((packed));

struct GSLEntryBE : GSLEntry<be_uint32_t> { };
struct GSLEntryLE : GSLEntry<le_uint32_t> { };

template <typename EntryT, size_t EntryCount>
void extract_gsl_t(const char* filename) {
  auto f = fopen_unique(filename, "rb");

  array<EntryT, EntryCount> entries;
  freadx(f.get(), entries.data(), sizeof(EntryT) * entries.size());

  for (const auto& entry : entries) {
    if (!entry.name[0]) {
      continue;
    }

    size_t size = entry.size;
    printf("> %s (0x%zX bytes)\n", entry.name, size);

    // File data is aligned on 2KB boundaries
    fseek(f.get(), (ftell(f.get()) + 0x7FF) & (~0x7FF), SEEK_SET);

    save_file(
        string_printf("%s-%s", filename, entry.name),
        freadx(f.get(), size));
  }
}

int main(int argc, char* argv[]) {
  const char* input_filename = nullptr;
  bool bb_format = false;
  for (int x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "--bb")) {
      bb_format = true;
    } else if (!input_filename) {
      input_filename = argv[x];
    } else {
      throw invalid_argument("excess option: " + string(argv[x]));
    }
  }
  if (!input_filename) {
    fputs("Usage: gsldump [--bb] <filename>\n", stderr);
    return 1;
  }

  if (bb_format) {
    extract_gsl_t<GSLEntryLE, 0x800>(input_filename);
  } else {
    extract_gsl_t<GSLEntryBE, 0x100>(input_filename);
  }

  return 0;
}
