#include <stdio.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <string>

using namespace std;

struct SequenceEntry {
  char name[14];
  uint16_t unknown1;
  uint32_t unknown2;
  uint32_t unknown3;
  uint32_t offset;
  uint32_t size;

  void byteswap() {
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
  }
};

int main(int argc, char** argv) {
  string index_data = load_file(argv[1]);
  SequenceEntry* seqs = reinterpret_cast<SequenceEntry*>(const_cast<char*>(index_data.data()));
  size_t num_seqs = index_data.size() / sizeof(SequenceEntry);

  {
    scoped_fd fd(argv[2], O_RDONLY);
    for (size_t x = 0; x < num_seqs; x++) {
      SequenceEntry* seq = &seqs[x];
      seq->byteswap();
      string data = preadx(fd, seq->size, seq->offset);
      save_file(string_printf("%s-%s", argv[1], seq->name), data);
    }
  }

  return 0;
}
