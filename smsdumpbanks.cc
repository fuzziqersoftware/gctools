// Super Mario Sunshine AAF disassembler
// heavily based on wwdumpsnd by hcs
// https://github.com/hcs64/vgm_ripping/blob/master/soundbank/wwdumpsnd/wwdumpsnd.c

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>

#include "wav.hh"

using namespace std;



vector<int16_t> afc_decode(const void* data, size_t size, bool small_frames) {
  static const int16_t coef[16][2] = {
    { 0x0000,  0x0000},
    { 0x0800,  0x0000},
    { 0x0000,  0x0800},
    { 0x0400,  0x0400},
    { 0x1000, -0x0800},
    { 0x0E00, -0x0600},
    { 0x0C00, -0x0400},
    { 0x1200, -0x0A00},
    { 0x1068, -0x08C8},
    { 0x12C0, -0x08FC},
    { 0x1400, -0x0C00},
    { 0x0800, -0x0800},
    { 0x0400, -0x0400},
    {-0x0400,  0x0400},
    {-0x0400,  0x0000},
    {-0x0800,  0x0000}};

  size_t frame_size = small_frames ? 5 : 9;
  if (size % frame_size != 0) {
    throw invalid_argument(string_printf(
        "input size (%zu) is not a multiple of frame size (%zu)",
        size, frame_size));
  }

  size_t frame_count = size / frame_size;
  size_t output_sample_count = frame_count * 16;

  int16_t history[2] = {0, 0};
  vector<int16_t> output_samples(output_sample_count, 0);
  for (size_t frame_index = 0; frame_index < frame_count; frame_index++) {
    const int8_t* frame_data = reinterpret_cast<const int8_t*>(data) +
        (frame_index * frame_size);

    int16_t delta = 1 << ((frame_data[0] >> 4) & 0x0F);
    int16_t coef_table_index = frame_data[0] & 0x0F;

    int16_t nibbles[16];
    if (!small_frames) {
      for (size_t x = 0; x < 8; x++) {
        nibbles[2 * x + 0] = (frame_data[x + 1] >> 4) & 0x0F;
        nibbles[2 * x + 1] = (frame_data[x + 1] >> 0) & 0x0F;
      }
      for (size_t x = 0; x < 16; x++) {
        if (nibbles[x] >= 8) {
          nibbles[x] = nibbles[x] - 16;
        }
        nibbles[x] <<= 11;
      }
    } else {
      for (size_t x = 0; x < 4; x++) {
        nibbles[4 * x + 0] = (frame_data[x + 1] >> 6) & 0x03;
        nibbles[4 * x + 1] = (frame_data[x + 1] >> 4) & 0x03;
        nibbles[4 * x + 2] = (frame_data[x + 1] >> 2) & 0x03;
        nibbles[4 * x + 3] = (frame_data[x + 1] >> 0) & 0x03;
      }
      for (size_t x = 0; x < 16; x++) {
        if (nibbles[x] >= 2) {
          nibbles[x] = nibbles[x] - 4;
        }
        nibbles[x] <<= 13;
      }
    }

    for (size_t x = 0; x < 16; x++) {
      int32_t sample = delta * nibbles[x] +
          (static_cast<int32_t>(history[0]) * coef[coef_table_index][0]) +
          (static_cast<int32_t>(history[1]) * coef[coef_table_index][1]);
      sample >>= 11;
      if (sample > 0x7FFF) {
        sample = 0x7FFF;
      }
      if (sample < -0x8000) {
        sample = -0x8000;
      }
      output_samples[16 * frame_index + x] = static_cast<int16_t>(sample);
      history[1] = history[0];
      history[0] = static_cast<int16_t>(sample);
    }
  }

  return output_samples;
}



struct aw_file_entry {
  char filename[112];
  uint32_t wav_count;
  uint32_t wav_entry_offsets[0];

  void byteswap() {
    this->wav_count = bswap32(this->wav_count);
  }
};

struct wave_table_entry {
  uint32_t flags1; // apparently
  uint32_t flags2;
  uint32_t offset;
  uint32_t size;

  uint32_t sample_rate() {
    return (this->flags2 >> 9) & 0x00007FFF;
  }

  uint32_t type() {
    return (this->flags1 >> 16) & 0xFF;
  }

  void byteswap() {
    this->flags1 = bswap32(this->flags1);
    this->flags2 = bswap32(this->flags2);
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
  }
};

void wsys_decode(const void* vdata, size_t size) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);
  if (memcmp(vdata, "WSYS", 4)) {
    throw invalid_argument("WSYS file not at expected offset");
  }

  uint32_t winf_offset = bswap32(*(reinterpret_cast<const uint32_t*>(vdata) + 4));
  const uint8_t* winf_data = data + winf_offset;
  if (memcmp(winf_data, "WINF", 4)) {
    throw invalid_argument("WINF file not at expected offset");
  }

  uint32_t aw_file_count = bswap32(*reinterpret_cast<const uint32_t*>(winf_data + 4));
  const uint32_t* table_offset_list = reinterpret_cast<const uint32_t*>(winf_data + 8);
  for (size_t x = 0; x < aw_file_count; x++) {

    const aw_file_entry* orig_entry = reinterpret_cast<const aw_file_entry*>(
        data + bswap32(table_offset_list[x]));
    aw_file_entry entry;
    memcpy(&entry, orig_entry, sizeof(aw_file_entry));
    entry.byteswap();

    string aw_file_contents = load_file(entry.filename);

    for (size_t x = 0; x < entry.wav_count; x++) {
      uint32_t wav_entry_offset = bswap32(orig_entry->wav_entry_offsets[x]);
      wave_table_entry wav_entry;
      memcpy(&wav_entry, data + wav_entry_offset, sizeof(wav_entry));
      wav_entry.byteswap();

      string output_filename = string_printf(
          "%s-%08zX-%08" PRIX32 "-%08" PRIX32 "-%08" PRIX32 "-%08"
          PRIX32 "-%" PRIu32 ".wav", entry.filename, x, wav_entry.flags1,
          wav_entry.flags2, wav_entry.offset, wav_entry.size,
          wav_entry.sample_rate());
      fprintf(stderr, "... %s\n", output_filename.c_str());

      vector<int16_t> samples;
      uint8_t type = wav_entry.type();
      if (type < 2) {
        samples = afc_decode(aw_file_contents.data() + wav_entry.offset,
            wav_entry.size, (type == 1));
        save_wav(output_filename.c_str(), samples, wav_entry.sample_rate(), 1);

      } else if (type == 3) {
        // uncompressed big-endian stereo?
        if (wav_entry.size & 3) {
          throw invalid_argument("data size not a multiple of 4");
        }
        samples.resize(wav_entry.size / 2);
        memcpy(const_cast<int16_t*>(samples.data()),
            aw_file_contents.data() + wav_entry.offset, wav_entry.size);
        for (auto& sample : samples) {
          sample = bswap16(sample);
        }
        save_wav(output_filename.c_str(), samples, wav_entry.sample_rate(), 2);
      }
    }
  }
}

void aaf_decode(const void* vdata, size_t size) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);
  size_t offset = 0;

  while (offset < size) {
    uint32_t chunk_offset, chunk_size, chunk_id;
    uint32_t chunk_type = bswap32(*reinterpret_cast<const uint32_t*>(data + offset));

    switch (chunk_type) {
      case 1:
      case 4:
      case 5:
      case 6:
      case 7:
        chunk_offset = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 4));
        chunk_size = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 8));
        // unused int32 after size apparently?
        offset += 0x10;
        break;

      case 2:
      case 3:
        offset += 0x04;
        while (offset < size) {
          chunk_offset = bswap32(*reinterpret_cast<const uint32_t*>(data + offset));
          if (chunk_offset == 0) {
            offset += 0x04;
            break;
          }

          chunk_size = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 4));
          chunk_id = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 8));
          if (chunk_type == 3) {
            wsys_decode(data + chunk_offset, chunk_size);
          }
          offset += 0x0C;
        }
        break;

      case 0:
        offset = size;
        break;

      default:
        throw invalid_argument("unknown chunk type");
    }
  }
}



int main(int argc, char ** argv) {
  string aaf_data = load_file(argv[1]);
  aaf_decode(aaf_data.data(), aaf_data.size());
}
