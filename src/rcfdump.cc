#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <string>
#include <unordered_map>
#include <vector>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>

#pragma pack(1)

using namespace std;


struct RCFHeader {
  char ident[0x20];
  uint32_t unknown;
  uint32_t index_offset;

  void byteswap() {
    this->index_offset = bswap32(this->index_offset);
  }
};

struct RCFIndexHeader {
  uint32_t count;
  uint32_t names_offset;
  uint32_t unknown[2];

  void byteswap() {
    this->count = bswap32(this->count);
    this->names_offset = bswap32(this->names_offset);
  }
};

struct RCFIndexEntry {
  uint32_t crc32;
  uint32_t offset;
  uint32_t size;

  void byteswap() {
    this->crc32 = bswap32(this->crc32);
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
  }
};


vector<string> parse_names_index(const string& data, size_t offset) {
  // For some reason this isn't reverse-endian... weird
  uint32_t num_names = *reinterpret_cast<const uint32_t*>(&data[offset]);
  offset += 8;

  vector<string> ret;
  while (ret.size() < num_names) {
    uint32_t len = *reinterpret_cast<const uint32_t*>(&data[offset]);
    ret.emplace_back(&data[offset + 4], len - 1);
    offset += (len + 8);
  }

  return ret;
}


unordered_map<string, RCFIndexEntry> get_index(const string& data, size_t offset) {
  RCFIndexHeader header;
  memcpy(&header, &data[offset], sizeof(RCFIndexHeader));
  header.byteswap();
  offset += sizeof(RCFIndexHeader);

  vector<string> names = parse_names_index(data, header.names_offset);
  if (header.count != names.size()) {
    throw runtime_error("name count and file count do not match");
  }

  unordered_map<string, RCFIndexEntry> ret;
  while (ret.size() < header.count) {
    const string& name = names.at(ret.size());
    auto& entry = ret[name];

    memcpy(&entry, &data[offset], sizeof(RCFIndexEntry));
    entry.byteswap();
    offset += sizeof(RCFIndexEntry);
  }

  return ret;
}


int main(int argc, char* argv[]) {

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
    return -1;
  }

  string data = load_file(argv[1]);
  RCFHeader header;
  memcpy(&header, data.data(), sizeof(RCFHeader));
  if (strcmp(header.ident, "RADCORE CEMENT LIBRARY")) {
    fprintf(stderr, "file does not appear to be an rcf archive\n");
    return 2;
  }

  header.byteswap();
  auto index = get_index(data, header.index_offset);

  for (const auto& it : index) {
    const string& name = it.first;
    const auto& entry = it.second;
    printf("... %08X %08X %08X %s\n", entry.crc32, entry.offset, entry.size,
        name.c_str());

    save_file(name, data.substr(entry.offset, entry.size));
  }

  return 0;
}
