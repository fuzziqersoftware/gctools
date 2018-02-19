#include "aaf.hh"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>
#include <map>

#include "instrument.hh"

using namespace std;



struct wave_table_entry {
  uint8_t unknown1;
  uint8_t type;
  uint8_t base_note;
  uint8_t unknown2;
  uint32_t flags2;
  uint32_t offset;
  uint32_t size;
  uint32_t loop_flag; // 0xFFFFFFFF means has a loop
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
  if (wsys->magic != 0x53595357) {
    throw invalid_argument("WSYS file not at expected offset");
  }
  wsys->byteswap();

  winf_header* winf = reinterpret_cast<winf_header*>(data + wsys->winf_offset);
  if (winf->magic != 0x464E4957) {
    throw invalid_argument("WINF file not at expected offset");
  }
  winf->byteswap();

  // get all sample IDs before processing aw files
  // this map is {(aw_file_index, wave_table_entry_index): sound_id}
  map<pair<size_t, size_t>, size_t> sound_ids;

  wbct_header* wbct = reinterpret_cast<wbct_header*>(data + wsys->wbct_offset);
  if (wbct->magic != 0x54434257) {
    throw invalid_argument("WBCT file not at expected offset");
  }
  wbct->byteswap();

  for (size_t x = 0; x < wbct->scne_count; x++) {
    scne_header* scne = reinterpret_cast<scne_header*>(data + wbct->scne_offsets[x]);
    if (scne->magic != 0x454E4353) {
      throw invalid_argument("SCNE file not at expected offset");
    }
    scne->byteswap();

    cdf_header* cdf = reinterpret_cast<cdf_header*>(data + scne->cdf_offset);
    if (cdf->magic != 0x46442D43) {
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

    // pikmin has a case where the aw filename is blank and the entry count is
    // zero. wtf? just handle it manually I guess
    if (entry->wav_count == 0) {
      continue;
    }

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
        ret_snd.afc_data = string(aw_file_contents.data() + wav_entry->offset,
            wav_entry->size);
        ret_snd.afc_large_frames = (wav_entry->type == 1);
        ret_snd.num_channels = 1;

      } else if (wav_entry->type < 4) {
        // uncompressed big-endian mono/stereo apparently
        bool is_stereo = (wav_entry->type == 3);
        if (is_stereo && (wav_entry->size & 3)) {
          throw invalid_argument("stereo data size not a multiple of 4");
        } else if (!is_stereo && (wav_entry->size & 2)) {
          throw invalid_argument("mono data size not a multiple of 2");
        }

        // hack: type 2 are too fast, so half their sample rate. I suspect they
        // might be stereo also, but then why are they a different type?
        if (wav_entry->type == 2) {
          ret_snd.sample_rate /= 2;
        }

        size_t num_samples = wav_entry->size / 2; // 16-bit samples
        ret_snd.decoded_samples.reserve(num_samples);
        const int16_t* samples = reinterpret_cast<const int16_t*>(
            aw_file_contents.data() + wav_entry->offset);
        for (size_t z = 0; z < num_samples; z++) {
          int16_t sample = bswap16(samples[z]);
          float decoded = (sample == -0x8000) ? -1.0 : (static_cast<float>(sample) / 32767.0f);
          ret_snd.decoded_samples.emplace_back(decoded);
        }
        ret_snd.num_channels = is_stereo ? 2 : 1;

      } else {
        throw runtime_error(string_printf("unknown wav entry type: 0x%" PRIX32, wav_entry->type));
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

unordered_map<string, SequenceProgram> barc_decode(void* vdata, size_t size,
    const char* base_directory) {

  barc_header* barc = reinterpret_cast<barc_header*>(vdata);
  if (barc->magic != 0x43524142) {
    throw invalid_argument("BARC file not at expected offset");
  }
  barc->byteswap();

  string sequence_archive_filename = string_printf("%s/Seqs/%s", base_directory,
      barc->archive_filename);
  scoped_fd sequence_archive_fd(sequence_archive_filename.c_str(), O_RDONLY);

  unordered_map<string, SequenceProgram> ret;
  for (size_t x = 0; x < barc->entry_count; x++) {
    const auto& e = barc->entries[x];
    ret.emplace(piecewise_construct, forward_as_tuple(e.name),
        forward_as_tuple(x, preadx(sequence_archive_fd, e.size, e.offset)));
  }

  return ret;
}



SequenceProgram::SequenceProgram(uint32_t index, std::string&& data) :
    index(index), data(move(data)) { }

void SoundEnvironment::resolve_pointers() {
  // postprocessing: resolve all sample bank pointers
  for (auto& bank_it : this->instrument_banks) {
    uint32_t bank_id = bank_it.first;
    auto& bank = bank_it.second;
    const auto& wsys_bank = this->sample_banks[bank.chunk_id];
    // fprintf(stderr, "[SoundEnvironment] bank %" PRIX32 " maps to wsys %" PRIX32 "\n",
    //     bank_id, bank.chunk_id);

    // build an index of sound_id -> index
    unordered_map<int64_t, size_t> sound_id_to_index;
    for (size_t x = 0; x < wsys_bank.size(); x++) {
      // fprintf(stderr, "[SoundEnvironment] wsys item %" PRIX32 ":%zX maps to sound %" PRIX64 "\n",
      //     bank.chunk_id, x, wsys_bank[x].sound_id);
      sound_id_to_index.emplace(wsys_bank[x].sound_id, x);

      // TODO: figure out if this is actually a problem and uncomment the code
      // below if it is
      /* if (!sound_id_to_index.emplace(wsys_bank[x].sound_id, x).second) {
        fprintf(stderr, "duplicate sound id %" PRIX64 " (indexes %zu and %zu)\n",
            wsys_bank[x].sound_id, sound_id_to_index[wsys_bank[x].sound_id], x);
      } */
    }

    // link all the VelocityRegions to their Sound objects
    for (auto& instrument_it : bank.id_to_instrument) {
      for (auto& key_region : instrument_it.second.key_regions) {
        for (auto& vel_region : key_region.vel_regions) {
          try {
            vel_region.sound = &wsys_bank[sound_id_to_index.at(vel_region.sound_id)];
          } catch (const out_of_range&) {
            fprintf(stderr, "[SoundEnvironment] error: can\'t resolve sound: bank=%" PRIX32
                " inst=%" PRIX32 " key_rgn=[%hhX,%hhX] vel_rgn=[%hhX, %hhX, base=%hhX, sound_id=%hX]\n",
                bank_id, instrument_it.first, key_region.key_low, key_region.key_high,
                vel_region.vel_low, vel_region.vel_high, vel_region.base_note,
                vel_region.sound_id);
          }
        }
      }
    }
  }
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
        ret.sequence_programs = barc_decode(data + chunk_offset, chunk_size, base_directory);
        offset += 0x10;
        break;

      case 0:
        offset = size;
        break;

      default:
        throw invalid_argument("unknown chunk type");
    }
  }

  ret.resolve_pointers();
  return ret;
}

struct bx_header {
  uint32_t wsys_table_offset;
  uint32_t wsys_count;
  uint32_t ibnk_table_offset;
  uint32_t ibnk_count;

  void byteswap() {
    this->wsys_table_offset = bswap32(this->wsys_table_offset);
    this->wsys_count = bswap32(this->wsys_count);
    this->ibnk_table_offset = bswap32(this->ibnk_table_offset);
    this->ibnk_count = bswap32(this->ibnk_count);
  }
};

struct bx_table_entry {
  uint32_t offset;
  uint32_t size;

  void byteswap() {
    this->offset = bswap32(this->offset);
    this->size = bswap32(this->size);
  }
};

SoundEnvironment bx_decode(void* vdata, size_t size, const char* base_directory) {
  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);
  bx_header* header = reinterpret_cast<bx_header*>(vdata);
  header->byteswap();

  SoundEnvironment ret;
  bx_table_entry* entry = reinterpret_cast<bx_table_entry*>(data + header->wsys_table_offset);
  for (size_t x = 0; x < header->wsys_count; x++) {
    entry->byteswap();
    if (entry->size == 0) {
      ret.sample_banks.emplace_back();
    } else {
      ret.sample_banks.emplace_back(wsys_decode(data + entry->offset,
          entry->size, base_directory));
    }
    entry++;
  }

  entry = reinterpret_cast<bx_table_entry*>(data + header->ibnk_table_offset);
  for (size_t x = 0; x < header->ibnk_count; x++) {
    entry->byteswap();
    if (entry->size != 0) {
      auto ibnk = ibnk_decode(data + entry->offset, entry->size);
      ibnk.chunk_id = x;
      ret.instrument_banks.emplace(x, move(ibnk));
    } else {
      ret.instrument_banks.emplace(x, x);
    }
    entry++;
  }

  ret.resolve_pointers();
  return ret;
}



SoundEnvironment load_sound_environment(const char* base_directory) {
  // Pikmin: pikibank.bx has almost everything; the sequence index is inside
  // default.dol (sigh) so it has to be manually extracted. search for 'BARC' in
  // default.dol in a hex editor and copy the resulting data (through the end of
  // the sequence names) to sequence.barc in the Seqs directory
  string filename = string_printf("%s/Banks/pikibank.bx", base_directory);
  if (isfile(filename)) {
    string data = load_file(filename);
    auto env = bx_decode(const_cast<char*>(data.data()), data.size(), base_directory);

    data = load_file(string_printf("%s/Seqs/sequence.barc", base_directory));
    env.sequence_programs = barc_decode(const_cast<char*>(data.data()), data.size(), base_directory);

    return env;
  }

  static const vector<string> filenames = {
    "/JaiInit.aaf",
    "/msound.aaf", // Super Mario Sunshine
  };

  for (const auto& filename : filenames) {
    string data;
    try {
      data = load_file(base_directory + filename);
    } catch (const cannot_open_file&) {
      continue;
    }

    return aaf_decode(const_cast<char*>(data.data()), data.size(), base_directory);
  }

  throw runtime_error("no index file found");
}
