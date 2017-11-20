#include "aaf.hh"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>
#include <map>

#include "../wav.hh"
#include "afc.hh"
#include "instrument.hh"

using namespace std;



/*

an aaf file is composed of multiple chunks, roughly in the following hierarchy:

- chunk headers, pointing to multiple top-level chunks
- ibnk_header (has bank id)
- - inst *
- - - TODO
- - per2 *
- - - per2_key_region {100}
- - - - TODO
- wsys_header (sampled sound information)
- - winf_header
- - - aw_file_entry *
- - - - wave_table_entry *
- - - - - offset, size, encoding of a sample block in a .aw file
- - wbct_header
- - - scne_header *
- - - - cdf_header
- - - - - cdf_record *
- - - - - - pointer to aw file by index, and sound id

*/



struct wave_table_entry {
  uint8_t unknown1;
  uint8_t type;
  uint8_t base_note;
  uint8_t unknown2;
  uint32_t flags2;
  uint32_t offset;
  uint32_t size;
  uint32_t loop_flag; // 0xFFFFFFFF means has a loop?
  uint32_t loop_start;
  uint32_t loop_end;
  uint32_t unknown3[4];

  uint32_t sample_rate() {
    return (this->flags2 >> 9) & 0x00007FFF;
  }

  void byteswap() {
    this->flags2 = bswap32(this->flags2);
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
    this->loop_flag = bswap32(this->loop_flag);
    this->loop_start = bswap32(this->loop_start);
    this->loop_end = bswap32(this->loop_end);
  }
};

struct aw_file_entry {
  char filename[112];
  uint32_t wav_count;
  uint32_t wav_entry_offsets[0];

  void byteswap() {
    this->wav_count = bswap32(this->wav_count);
    for (size_t x = 0; x < this->wav_count; x++) {
      this->wav_entry_offsets[x] = bswap32(this->wav_entry_offsets[x]);
    }
  }
};

struct winf_header {
  uint32_t magic; // 'WINF'
  uint32_t aw_file_count;
  uint32_t aw_file_entry_offsets[0];

  void byteswap() {
    this->aw_file_count = bswap32(this->aw_file_count);
    for (size_t x = 0; x < this->aw_file_count; x++) {
      this->aw_file_entry_offsets[x] = bswap32(this->aw_file_entry_offsets[x]);
    }
  }
};

struct cdf_record {
  uint16_t aw_file_index;
  uint16_t sound_id;
  uint32_t unknown1[13];

  void byteswap() {
    this->aw_file_index = bswap16(this->aw_file_index);
    this->sound_id = bswap16(this->sound_id);
  }
};

struct cdf_header {
  uint32_t magic; // 'C-DF'
  uint32_t record_count;
  uint32_t record_offsets[0];

  void byteswap() {
    this->record_count = bswap32(this->record_count);
    for (size_t x = 0; x < this->record_count; x++) {
      this->record_offsets[x] = bswap32(this->record_offsets[x]);
    }
  }
};

struct scne_header {
  uint32_t magic; // 'SCNE'
  uint32_t unknown1[2];
  uint32_t cdf_offset;

  void byteswap() {
    this->cdf_offset = bswap32(this->cdf_offset);
  }
};

struct wbct_header {
  uint32_t magic; // 'WBCT'
  uint32_t unknown1;
  uint32_t scne_count;
  uint32_t scne_offsets[0];

  void byteswap() {
    this->scne_count = bswap32(this->scne_count);
    for (size_t x = 0; x < this->scne_count; x++) {
      this->scne_offsets[x] = bswap32(this->scne_offsets[x]);
    }
  }
};

struct wsys_header {
  uint32_t magic; // 'WSYS'
  uint32_t size;
  uint32_t unknown1[2];
  uint32_t winf_offset;
  uint32_t wbct_offset;

  void byteswap() {
    this->size = bswap32(this->size);
    this->winf_offset = bswap32(this->winf_offset);
    this->wbct_offset = bswap32(this->wbct_offset);
  }
};



vector<Sound> wsys_decode(void* vdata, size_t size,
    const char* base_directory) {
  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);

  wsys_header* wsys = reinterpret_cast<wsys_header*>(data);
  if (wsys->magic != 'SYSW') {
    throw invalid_argument("WSYS file not at expected offset");
  }
  wsys->byteswap();

  winf_header* winf = reinterpret_cast<winf_header*>(data + wsys->winf_offset);
  if (winf->magic != 'FNIW') {
    throw invalid_argument("WINF file not at expected offset");
  }
  winf->byteswap();

  // get all sample IDs before processing aw files
  // this map is {(aw_file_index, wave_table_entry_index): sound_id}
  map<pair<size_t, size_t>, size_t> sound_ids;

  wbct_header* wbct = reinterpret_cast<wbct_header*>(data + wsys->wbct_offset);
  if (wbct->magic != 'TCBW') {
    throw invalid_argument("WBCT file not at expected offset");
  }
  wbct->byteswap();

  for (size_t x = 0; x < wbct->scne_count; x++) {
    scne_header* scne = reinterpret_cast<scne_header*>(data + wbct->scne_offsets[x]);
    if (scne->magic != 'ENCS') {
      throw invalid_argument("SCNE file not at expected offset");
    }
    scne->byteswap();

    cdf_header* cdf = reinterpret_cast<cdf_header*>(data + scne->cdf_offset);
    if (cdf->magic != 'FD-C') {
      throw invalid_argument("C-DF file not at expected offset");
    }
    cdf->byteswap();

    for (size_t y = 0; y < cdf->record_count; y++) {
      cdf_record* record = reinterpret_cast<cdf_record*>(data + cdf->record_offsets[y]);
      record->byteswap();

      sound_ids.emplace(make_pair(record->aw_file_index, y), record->sound_id);
    }
  }

  // now process aw files
  vector<Sound> ret;
  for (size_t x = 0; x < winf->aw_file_count; x++) {
    aw_file_entry* entry = reinterpret_cast<aw_file_entry*>(
        data + winf->aw_file_entry_offsets[x]);
    entry->byteswap();

    string aw_filename = string_printf("%s/Banks/%s", base_directory,
        entry->filename);
    string aw_file_contents = load_file(aw_filename.c_str());

    for (size_t y = 0; y < entry->wav_count; y++) {
      wave_table_entry* wav_entry = reinterpret_cast<wave_table_entry*>(
          data + entry->wav_entry_offsets[y]);
      wav_entry->byteswap();

      uint16_t sound_id = sound_ids.at(make_pair(x, y));

      ret.emplace_back();
      Sound& ret_snd = ret.back();
      ret_snd.sample_rate = wav_entry->sample_rate();
      ret_snd.base_note = wav_entry->base_note;
      if (wav_entry->loop_flag == 0xFFFFFFFF) {
        ret_snd.loop_start = wav_entry->loop_start;
        ret_snd.loop_end = wav_entry->loop_end;
      } else {
        ret_snd.loop_start = 0;
        ret_snd.loop_end = 0;
      }

      ret_snd.source_filename = entry->filename;
      ret_snd.source_offset = wav_entry->offset;
      ret_snd.source_size = wav_entry->size;

      ret_snd.aw_file_index = x;
      ret_snd.wave_table_index = y;
      ret_snd.sound_id = sound_id;

      if (wav_entry->type < 2) {
        vector<int16_t> int_samples = afc_decode(
            aw_file_contents.data() + wav_entry->offset, wav_entry->size,
            (wav_entry->type == 1));
        ret_snd.samples = convert_samples_to_float(int_samples);
        ret_snd.num_channels = 1;

      } else if (wav_entry->type == 3) {
        // uncompressed big-endian stereo apparently
        if (wav_entry->size & 3) {
          throw invalid_argument("data size not a multiple of 4");
        }

        size_t num_samples = wav_entry->size / 2;
        ret_snd.samples.reserve(num_samples);
        const int16_t* samples = reinterpret_cast<const int16_t*>(
            aw_file_contents.data() + wav_entry->offset);
        for (size_t z = 0; z < num_samples; z++) {
          if (samples[z] == 0x0080) {
            ret_snd.samples.emplace_back(-1.0f);
          } else {
            ret_snd.samples.emplace_back(
                static_cast<float>(bswap16(samples[z])) / 32767.0f);
          }
        }
        ret_snd.num_channels = 2;
      }
    }
  }

  return ret;
}



struct barc_entry {
  char name[14];
  uint16_t unknown1;
  uint32_t unknown2[2];
  uint32_t offset;
  uint32_t size;

  void byteswap() {
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
  }
};

struct barc_header {
  uint32_t magic; // 'BARC'
  uint32_t unknown1; // '----'
  uint32_t unknown2;
  uint32_t entry_count;
  char archive_filename[0x10];
  barc_entry entries[0];

  void byteswap() {
    this->entry_count = bswap32(this->entry_count);
    for (size_t x = 0; x < this->entry_count; x++) {
      this->entries[x].byteswap();
    }
  }
};

unordered_map<string, string> barc_decode(void* vdata, size_t size,
    const char* base_directory) {

  barc_header* barc = reinterpret_cast<barc_header*>(vdata);
  if (barc->magic != 'CRAB') {
    throw invalid_argument("BARC file not at expected offset");
  }
  barc->byteswap();

  string sequence_archive_filename = string_printf("%s/Seqs/%s", base_directory,
      barc->archive_filename);
  scoped_fd sequence_archive_fd(sequence_archive_filename.c_str(), O_RDONLY);

  unordered_map<string, string> ret;
  for (size_t x = 0; x < barc->entry_count; x++) {
    const auto& e = barc->entries[x];
    ret.emplace(e.name, preadx(sequence_archive_fd, e.size, e.offset));
  }

  return ret;
}



SoundEnvironment aaf_decode(void* vdata, size_t size, const char* base_directory) {
  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);
  size_t offset = 0;

  SoundEnvironment ret;
  while (offset < size) {
    uint32_t chunk_offset, chunk_size, chunk_id;
    uint32_t chunk_type = bswap32(*reinterpret_cast<const uint32_t*>(data + offset));

    switch (chunk_type) {
      case 1:
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
          if (chunk_type == 2) {
            auto ibnk = ibnk_decode(data + chunk_offset, chunk_size);
            // this is the index of the related wsys block
            ibnk.chunk_id = chunk_id;
            ret.instrument_banks.emplace(ibnk.id, move(ibnk));
          } else {
            ret.sample_banks.emplace_back(wsys_decode(
                data + chunk_offset, chunk_size, base_directory));
          }
          offset += 0x0C;
        }
        break;

      case 4:
        chunk_offset = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 4));
        chunk_size = bswap32(*reinterpret_cast<const uint32_t*>(data + offset + 8));
        for (auto& it : barc_decode(data + chunk_offset, chunk_size, base_directory)) {
          ret.sequence_programs.emplace(it.first, it.second);
        }
        offset += 0x10;
        break;

      case 0:
        offset = size;
        break;

      default:
        throw invalid_argument("unknown chunk type");
    }
  }

  // postprocessing: resolve all sample bank pointers
  for (auto& bank_it : ret.instrument_banks) {
    const auto& wsys_bank = ret.sample_banks[bank_it.second.chunk_id];

    // build an index of sound_id -> index
    unordered_map<int64_t, size_t> sound_id_to_index;
    for (size_t x = 0; x < wsys_bank.size(); x++) {
      sound_id_to_index.emplace(wsys_bank[x].sound_id, x);

      // TODO: figure out if this is actually a problem and uncomment the code
      // below if it is
      /* if (!sound_id_to_index.emplace(wsys_bank[x].sound_id, x).second) {
        fprintf(stderr, "duplicate sound id %" PRIX64 " (indexes %zu and %zu)\n",
            wsys_bank[x].sound_id, sound_id_to_index[wsys_bank[x].sound_id], x);
      } */
    }

    // link all the VelocityRegions to their Sound objects
    for (auto& instrument_it : bank_it.second.id_to_instrument) {
      for (auto& key_region : instrument_it.second.key_regions) {
        for (auto& vel_region : key_region.vel_regions) {
          if (vel_region.sound_id < wsys_bank.size()) {
            size_t index = sound_id_to_index[vel_region.sound_id];
            vel_region.sound = &wsys_bank[index];
          } else {
            fprintf(stderr, "[AAF] error: can\'t resolve sound id %" PRIX32 "\n",
                vel_region.sound_id);
          }
        }
      }
    }
  }

  return ret;
}

SoundEnvironment aaf_decode_directory(const char* base_directory) {
  string aaf_data = load_file(string_printf("%s/msound.aaf", base_directory));
  return aaf_decode(const_cast<char*>(aaf_data.data()), aaf_data.size(),
      base_directory);
}
