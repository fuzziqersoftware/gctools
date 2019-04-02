#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>

using namespace std;



struct AFSHeader {
  uint32_t magic;
  uint32_t num_files;
  struct {
    uint32_t offset;
    uint32_t size;
  } entries[0xFFFE];
  uint32_t filename_offset;
  uint32_t unknown;
};



int main(int argc, char* argv[]) {

  if (argc != 2) {
    fprintf(stderr, "usage: %s <filename>\n", argv[0]);
    return 1;
  }

  string data = load_file(argv[1]);

  const AFSHeader* header = reinterpret_cast<const AFSHeader*>(data.data());
  if (header->magic != 0x00534641) {
    fprintf(stderr, "file does not appear to be an afs archive\n");
    return 2;
  }

  uint32_t filename_offset = header->filename_offset;
  for (size_t x = 0; x < header->num_files; x++) {
    string output_filename;
    if (filename_offset) {
      const char* internal_filename = data.data() + (filename_offset + 0x30 * x);
      output_filename = string_printf("%s-%-.32s", argv[1], internal_filename);
    } else {
      output_filename = string_printf("%s-%zu", argv[1], x);
    }

    if (header->entries[x].offset + header->entries[x].size > data.size()) {
      throw runtime_error("file size exceeds archive boundary");
    }

    {
      scoped_fd fd(output_filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      writex(fd, data.data() + header->entries[x].offset, header->entries[x].size);
    }

    printf("... %s\n", output_filename.c_str());
  }

  return 0;
}
