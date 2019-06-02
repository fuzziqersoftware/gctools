#include "instrument.hh"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>

#include "afc.hh"

using namespace std;



const vector<float>& Sound::samples() const {
  if (this->decoded_samples.empty()) {
    this->decoded_samples = afc_decode(this->afc_data.data(),
        this->afc_data.size(), this->afc_large_frames);
    this->afc_data.clear();
  }

  return this->decoded_samples;
}



VelocityRegion::VelocityRegion(uint8_t vel_low, uint8_t vel_high,
    uint16_t sample_bank_id, uint16_t sound_id, float freq_mult,
    float volume_mult, int8_t base_note, bool constant_pitch) :
    vel_low(vel_low), vel_high(vel_high), sample_bank_id(sample_bank_id),
    sound_id(sound_id), freq_mult(freq_mult), volume_mult(volume_mult),
    constant_pitch(constant_pitch), base_note(base_note), sound(NULL) { }

KeyRegion::KeyRegion(uint8_t key_low, uint8_t key_high) : key_low(key_low),
    key_high(key_high) { }

const VelocityRegion& KeyRegion::region_for_velocity(uint8_t velocity) const {
  for (const VelocityRegion& r : this->vel_regions) {
    if (r.vel_low <= velocity && r.vel_high >= velocity) {
      return r;
    }
  }
  throw out_of_range("no such velocity");
}

Instrument::Instrument(uint32_t id) : id(id) { }

const KeyRegion& Instrument::region_for_key(uint8_t key) const {
  for (const KeyRegion& r : this->key_regions) {
    if (r.key_low <= key && r.key_high >= key) {
      return r;
    }
  }
  throw out_of_range("no such key");
}

InstrumentBank::InstrumentBank(uint32_t id) : id(id), chunk_id(0) { }



struct inst_vel_region {
  uint8_t vel_high;
  uint8_t unknown1[3];
  uint16_t sample_bank_id;
  uint16_t sample_num;
  float volume_mult;
  float freq_mult;

  void byteswap() {
    this->sample_bank_id = bswap16(this->sample_bank_id);
    this->sample_num = bswap16(this->sample_num);
    this->volume_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->volume_mult));
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
  }
};

struct inst_key_region {
  uint8_t key_high;
  uint8_t unknown1[3];
  uint32_t vel_region_count;
  uint32_t vel_region_offsets[0];

  void byteswap() {
    this->vel_region_count = bswap32(this->vel_region_count);
    for (size_t x = 0; x < this->vel_region_count; x++) {
      this->vel_region_offsets[x] = bswap32(this->vel_region_offsets[x]);
    }
  }
};

struct inst_header {
  uint32_t magic;
  uint32_t unknown;
  float freq_mult;
  float volume_mult;
  uint32_t osc_offsets[2];
  uint32_t eff_offsets[2];
  uint32_t sen_offsets[2];
  uint32_t key_region_count;
  uint32_t key_region_offsets[0];

  void byteswap() {
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
    this->volume_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->volume_mult));
    this->osc_offsets[0] = bswap32(this->osc_offsets[0]);
    this->osc_offsets[1] = bswap32(this->osc_offsets[1]);
    this->eff_offsets[0] = bswap32(this->eff_offsets[0]);
    this->eff_offsets[1] = bswap32(this->eff_offsets[1]);
    this->sen_offsets[0] = bswap32(this->sen_offsets[0]);
    this->sen_offsets[1] = bswap32(this->sen_offsets[1]);
    this->key_region_count = bswap32(this->key_region_count);
    for (size_t x = 0; x < this->key_region_count; x++) {
      this->key_region_offsets[x] = bswap32(this->key_region_offsets[x]);
    }
  }
};

struct per2_key_region {
  float freq_mult;
  float volume_mult;
  uint32_t unknown2[2];
  uint32_t vel_region_count;
  uint32_t vel_region_offsets[0];

  void byteswap() {
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
    this->volume_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->volume_mult));
    this->vel_region_count = bswap32(this->vel_region_count);
    for (size_t x = 0; x < this->vel_region_count; x++) {
      this->vel_region_offsets[x] = bswap32(this->vel_region_offsets[x]);
    }
  }
};

struct per2_header {
  uint32_t magic;
  uint32_t unknown1[0x21];
  uint32_t key_region_offsets[100];

  void byteswap() {
    for (size_t x = 0; x < 100; x++) {
      this->key_region_offsets[x] = bswap32(this->key_region_offsets[x]);
    }
  }
};

struct perc_header {
  // total guess: PERC instruments are just 0x7F key regions after the magic
  // number. there don't appear to be any size/count fields in the structure.
  // another guess: the key region format appears to match the per2 key region
  // format; assume they're the same
  uint32_t magic;
  uint32_t key_region_offsets[0x7F];

  void byteswap() {
    for (size_t x = 0; x < 0x7F; x++) {
      this->key_region_offsets[x] = bswap32(this->key_region_offsets[x]);
    }
  }
};

struct ins_bank {
  uint32_t magic; // 'IBNK'
  uint32_t size;
  uint32_t bank_id;
  uint32_t unknown1[5];
  uint32_t bank_magic; // 'BANK'
  uint32_t inst_offsets[245];

  void byteswap() {
    this->size = bswap32(this->size);
    this->bank_id = bswap32(this->bank_id);
    for (size_t x = 0; x < 245; x++) {
      this->inst_offsets[x] = bswap32(this->inst_offsets[x]);
    }
  }
};

InstrumentBank ibnk_decode(void* vdata) {
  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);
  if (memcmp(vdata, "IBNK", 4)) {
    throw invalid_argument("IBNK file not at expected offset");
  }

  ins_bank* bank = reinterpret_cast<ins_bank*>(vdata);
  bank->byteswap();

  InstrumentBank result_bank(bank->bank_id);

  uint8_t* bank_data = data + 0x20;
  if (memcmp(bank_data, "BANK", 4)) {
    throw invalid_argument("BANK file not at expected offset");
  }

  for (size_t z = 0; z < 245; z++) {
    if (!bank->inst_offsets[z]) {
      continue;
    }

    auto& result_inst = result_bank.id_to_instrument.emplace(piecewise_construct,
        forward_as_tuple(z), forward_as_tuple(z)).first->second;

    // decode instrument struct at vdata + ins_offset
    // TODO: apparently instrument numbers are (x & 0x7F)
    uint8_t* inst_data = data + bank->inst_offsets[z];

    if (!memcmp(inst_data, "INST", 4)) {
      inst_header* inst = reinterpret_cast<inst_header*>(inst_data);

      // TODO: remove debugging code here
      // string s_data = format_data_string(string(reinterpret_cast<char*>(inst), sizeof(*inst)));
      // fprintf(stderr, "INST %04zX -> %s\n", z, s_data.c_str());

      inst->byteswap();

      if (inst->freq_mult == 0) {
        inst->freq_mult = 1;
      }
      if (inst->volume_mult == 0) {
        inst->volume_mult = 1;
      }

      uint8_t key_low = 0;
      for (uint32_t x = 0; x < inst->key_region_count; x++) {
        inst_key_region* key_region = reinterpret_cast<inst_key_region*>(
            data + inst->key_region_offsets[x]);
        key_region->byteswap();

        result_inst.key_regions.emplace_back(key_low, key_region->key_high);
        auto& result_key_region = result_inst.key_regions.back();

        uint8_t vel_low = 0;
        for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
          inst_vel_region* vel_region = reinterpret_cast<inst_vel_region*>(
              data + key_region->vel_region_offsets[y]);
          vel_region->byteswap();

          // TODO: we should also multiply by inst->freq_mult here, but it makes
          // Sunshine sequences sound wrong (especially k_dolpic). figure out
          // why and fix it
          result_key_region.vel_regions.emplace_back(vel_low,
              vel_region->vel_high, vel_region->sample_bank_id,
              vel_region->sample_num, vel_region->freq_mult,
              vel_region->volume_mult * inst->volume_mult);

          vel_low = vel_region->vel_high + 1;
        }
        key_low = key_region->key_high + 1;
      }
      continue;
    }

    uint32_t* offset_table = NULL;
    uint32_t count = 0;
    if (!memcmp(inst_data, "PERC", 4)) {
      perc_header* perc = reinterpret_cast<perc_header*>(inst_data);

      // TODO: remove debugging code here
      // string data = format_data_string(string(reinterpret_cast<char*>(perc), sizeof(*perc)));
      // fprintf(stderr, "PERC %04zX -> %s\n", z, data.c_str());

      perc->byteswap();
      offset_table = perc->key_region_offsets;
      count = 0x7F;

    } else if (!memcmp(inst_data, "PER2", 4)) {
      per2_header* per2 = reinterpret_cast<per2_header*>(inst_data);

      // TODO: remove debugging code here
      // string data = format_data_string(string(reinterpret_cast<char*>(per2), sizeof(*per2)));
      // fprintf(stderr, "PER2 %04zX -> %s\n", z, data.c_str());

      per2->byteswap();
      offset_table = per2->key_region_offsets;
      count = 0x64;

    } else {
      throw invalid_argument(string_printf("unknown instrument format: %4s", inst_data));
    }

    for (uint32_t x = 0; x < count; x++) {
      if (!offset_table[x]) {
        continue;
      }

      per2_key_region* key_region = reinterpret_cast<per2_key_region*>(
          data + offset_table[x]);
      key_region->byteswap();

      result_inst.key_regions.emplace_back(x, x);
      auto& result_key_region = result_inst.key_regions.back();

      uint8_t vel_low = 0;
      for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
        inst_vel_region* vel_region = reinterpret_cast<inst_vel_region*>(
            data + key_region->vel_region_offsets[y]);
        vel_region->byteswap();

        // TODO: Luigi's Mansion appears to multiply these by 8. figure out
        // where this comes from and implement it properly (right now we don't
        // implement it, because Pikmin doesn't do this and it sounds terrible
        // if we do)
        float freq_mult = vel_region->freq_mult * key_region->freq_mult;
        result_key_region.vel_regions.emplace_back(vel_low,
            vel_region->vel_high, vel_region->sample_bank_id,
            vel_region->sample_num, freq_mult, 1.0, x);

        vel_low = vel_region->vel_high + 1;
      }
    }
  }

  return result_bank;
}
