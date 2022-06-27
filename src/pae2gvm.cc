#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

#include "prs/PRS.hh"

using namespace std;

struct PAEHeader {
  be_int32_t unknown1;
  be_int32_t decompressed_size;
  be_int32_t unknown2[5];
  be_int32_t gvm_offset;
} __attribute__((packed));

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return 1;
  }

  const char* input_filename = argv[1];
  auto pae = fopen_unique(input_filename, "rb");

  PAEHeader header;
  freadx(pae.get(), &header, sizeof(PAEHeader));

  string out_filename = string_printf("%s.dec", input_filename);
  auto dec = fopen_unique(out_filename, "w+b");

  fwritex(dec.get(), &header, sizeof(PAEHeader));
  prs_decompress_stream(pae.get(), dec.get(), 0);
  pae.reset(); // Calls fclose()

  fseek(dec.get(), header.gvm_offset + sizeof(PAEHeader), SEEK_SET);
  save_file(
      string_printf("%s.gvm", input_filename),
      read_all(dec.get()));

  return 0;
}
