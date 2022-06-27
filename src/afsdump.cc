#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

using namespace std;



struct AFSHeader {
  uint32_t magic;
  uint32_t num_files;
  struct {
    uint32_t offset;
    uint32_t size;
  } entries[0];
};



int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return 1;
  }

  string data = load_file(argv[1]);

  const AFSHeader* header = reinterpret_cast<const AFSHeader*>(data.data());
  if (header->magic != 0x00534641) {
    throw runtime_error("file does not appear to be an AFS archive");
  }

  for (size_t x = 0; x < header->num_files; x++) {
    if (header->entries[x].offset + header->entries[x].size > data.size()) {
      throw runtime_error("file size exceeds archive boundary");
    }

    string output_filename = string_printf("%s-%zu", argv[1], x);
    save_file(
        output_filename,
        data.data() + header->entries[x].offset,
        header->entries[x].size);
    fprintf(stderr, "... %s\n", output_filename.c_str());
  }

  return 0;
}
