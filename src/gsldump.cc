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

using namespace std;

struct GSLEntry {
  char name[0x20];
  be_int32_t unknown;
  be_uint32_t size;
  be_int32_t unknown2[2];
} __attribute__((packed));

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return -1;
  }

  const char* input_filename = argv[1];
  auto f = fopen_unique(input_filename, "rb");

  array<GSLEntry, 0x100> entries;
  freadx(f.get(), entries.data(), sizeof(GSLEntry) * entries.size());

  for (const auto& entry : entries) {
    if (!entry.name[0]) {
      continue;
    }

    size_t size = entry.size;
    printf("> %s (0x%zX bytes)\n", entry.name, size);

    // File data is aligned on 2KB boundaries
    fseek(f.get(), (ftell(f.get()) + 0x7FF) & (~0x7FF), SEEK_SET);

    save_file(
        string_printf("%s-%s", input_filename, entry.name),
        freadx(f.get(), size));
  }

  return 0;
}
