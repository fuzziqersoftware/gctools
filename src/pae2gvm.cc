#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

#include "prs/PRS.hh"

using namespace std;

struct PAEHeader {
  be_uint32_t unknown_a1;
  be_uint32_t decompressed_size;
  be_uint32_t unknown_a2;
  be_uint32_t partition_table_offset; // Not including this header (so +0x20 == actual offset in file)
  be_uint32_t partition_table_size;
  be_uint32_t string_table_offset; // Not including this header (so +0x20 == actual offset in file)
  be_uint32_t string_table_size;
  be_uint32_t gvm_offset; // Not including this header (so +0x20 == actual offset in file)
} __attribute__((packed));

struct PAEStringTableHeader {
  be_uint32_t unknown_a1;
  be_uint32_t num_entries;
} __attribute__((packed));

struct PAEStringTableEntry {
  be_uint32_t offset; // Relative to start of PAEStringTableHeader
  be_uint32_t unknown_a1;
  be_uint32_t unknown_a2;
} __attribute__((packed));

struct PAEPartitionTable {
  be_uint32_t unknown_a1;
  be_uint32_t floats_table_offset; // Relative to start of this struct
  // Variable-length fields:
  // PAEPartitionEntry[...]; // Number of these is determined by the minimum sub_entry_offset in any float table entry (or if there's only a null entry in the float table, then there are as many of these as can fill the space before the float table)
  // PAEFloatTableSubEntry[...];
  // PAEFloatTableEntry[...];
} __attribute__((packed));

struct PAEFloatTableSubEntry {
  be_float unknown_a1[6]; // TODO: The first and last entries here might not be floats (they seem to alwyas be zero)
} __attribute__((packed));

struct PAEFloatTableEntry {
  be_uint16_t type;
  be_uint16_t partition_number;
  be_uint32_t unknown_a1;
  be_float unknown_a2[3];
  be_uint32_t unknown_a3[2];
  be_float unknown_a4;
  be_uint32_t unknown_a5;
  be_uint32_t sub_entry_offset1; // Relative to start of PAEPartitionTable
  be_uint32_t unknown_a6[2];
  be_uint32_t sub_entry_offset2; // Relative to start of PAEPartitionTable
  be_uint32_t unknown_a7[2];
} __attribute__((packed));

struct PAEPartitionEntry {
  be_uint16_t texture_index; // Probably index into string table, which specifies (by name) which GVR to read from
  be_uint16_t scaled_x; // X position as if images was 256x256
  be_uint16_t scaled_y; // Y position as if images was 256x256
  be_uint16_t scaled_width; // Width as if image was 256x256
  be_uint16_t scaled_height; // Height as if image was 256x256
  be_uint16_t width; // Actual width in pixels
  be_uint16_t height; // Actual height in pixels
  be_uint16_t unknown_a1; // Possibly flags? Seems to usually be 0 or 2
} __attribute__((packed));

// TODO: Implement PAE splitting. Also generate a stencil image showing the parts of the GVR that aren't part of any partition

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
