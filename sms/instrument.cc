#include "instrument.hh"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>

using namespace std;



VelocityRegion::VelocityRegion(uint8_t vel_low, uint8_t vel_high,
    uint16_t sound_id, float freq_mult, int8_t base_note) : vel_low(vel_low),
    vel_high(vel_high), sound_id(sound_id), freq_mult(freq_mult),
    base_note(base_note), sound(NULL) { }

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

InstrumentBank::InstrumentBank(uint32_t id) : id(id) { }



struct inst_vel_region {
  uint8_t vel_high;
  uint8_t unknown1[5];
  uint16_t sample_num;
  float unknown2;
  float freq_mult;

  void byteswap() {
    this->sample_num = bswap16(this->sample_num);
    this->unknown2 = bswap32f(*reinterpret_cast<uint32_t*>(&this->unknown2));
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
  uint32_t unknown1[9];
  uint32_t key_region_count;
  uint32_t key_region_offsets[0];

  void byteswap() {
    this->key_region_count = bswap32(this->key_region_count);
    for (size_t x = 0; x < this->key_region_count; x++) {
      this->key_region_offsets[x] = bswap32(this->key_region_offsets[x]);
    }
  }
};

struct per2_key_region {
  uint32_t unknown1;
  float freq_mult;
  uint32_t unknown2[2];
  uint32_t vel_region_count;
  uint32_t vel_region_offsets[0];

  void byteswap() {
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
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

InstrumentBank ibnk_decode(void* vdata, size_t size) {
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

    auto& result_inst = result_bank.id_to_instrument.emplace(z, z).first->second;

    // decode instrument struct at vdata + ins_offset
    // TODO: apparently instrument numbers are (x & 0x7F)
    uint8_t* inst_data = data + bank->inst_offsets[z];
    if (!memcmp(inst_data, "INST", 4)) {
      inst_header* inst = reinterpret_cast<inst_header*>(inst_data);
      inst->byteswap();

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

          result_key_region.vel_regions.emplace_back(vel_low,
              vel_region->vel_high, vel_region->sample_num,
              vel_region->freq_mult);

          vel_low = vel_region->vel_high + 1;
        }
        key_low = key_region->key_high + 1;
      }

    } else if (!memcmp(inst_data, "PER2", 4)) {

      per2_header* per2 = reinterpret_cast<per2_header*>(inst_data);
      per2->byteswap();

      for (uint32_t x = 0; x < 100; x++) {
        if (!per2->key_region_offsets[x]) {
          continue;
        }
        per2_key_region* key_region = reinterpret_cast<per2_key_region*>(
            data + per2->key_region_offsets[x]);
        key_region->byteswap();

        result_inst.key_regions.emplace_back(x, x);
        auto& result_key_region = result_inst.key_regions.back();

        uint8_t vel_low = 0;
        for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
          inst_vel_region* vel_region = reinterpret_cast<inst_vel_region*>(
              data + key_region->vel_region_offsets[y]);
          vel_region->byteswap();

          float freq_mult = vel_region->freq_mult * key_region->freq_mult;
          result_key_region.vel_regions.emplace_back(vel_low,
              vel_region->vel_high, vel_region->sample_num, freq_mult, x);

          vel_low = vel_region->vel_high + 1;
        }
      }

    } else {
      throw invalid_argument("unknown instrument format");
    }
  }
  return result_bank;
}
