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

#pragma pack(1)



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



struct ibnk_inst_inst_vel_region {
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

struct ibnk_inst_inst_key_region {
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

struct ibnk_inst_inst_header {
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

struct ibnk_inst_per2_key_region {
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

struct ibnk_inst_per2_header {
  uint32_t magic;
  uint32_t unknown1[0x21];
  uint32_t key_region_offsets[100];

  void byteswap() {
    for (size_t x = 0; x < 100; x++) {
      this->key_region_offsets[x] = bswap32(this->key_region_offsets[x]);
    }
  }
};

struct ibnk_inst_perc_header {
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

struct ibnk_inst_percnew_header {
  uint32_t magic; // 'Perc'
  uint32_t count;
  uint32_t pmap_offsets[0];

  void byteswap() {
    this->count = bswap32(this->count);
    for (size_t x = 0; x < count; x++) {
      this->pmap_offsets[x] = bswap32(this->pmap_offsets[x]);
    }
  }
};

struct ibnk_inst_instnew_vel_region {
  uint8_t vel_high;
  uint8_t unused[3];
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

struct ibnk_inst_instnew_key_region {
  uint8_t key_high;
  uint8_t unused[3];
  uint32_t vel_region_count;

  void byteswap() {
    this->vel_region_count = bswap32(this->vel_region_count);
  }
};

struct ibnk_inst_instnew_header {
  uint32_t magic; // 'Inst'
  uint32_t osc;
  uint32_t inst_id;
  // TODO: this appears to control the instrument format somehow. usually it's
  // zero but if it's a small number, it appears to specify the number of 32-bit
  // fields following it (of unknown purpose) before the key region count. for
  // example, Twilight Princess has an instrument that looks like this:
  // 496E7374 00000001 00000021 00000002 000014D8 00001518 00000003 3D000000 ...
  // (the 3D is probably key_high for the first key region)
  uint32_t unknown1;
  uint32_t key_region_count;

  void byteswap() {
    this->osc = bswap32(this->osc);
    this->inst_id = bswap32(this->inst_id);
    this->key_region_count = bswap32(this->key_region_count);
  }
};

struct ibnk_inst_instnew_footer {
  float volume_mult;
  float freq_mult;

  void byteswap() {
    this->volume_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->volume_mult));
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
  }
};

struct ibnk_inst_pmap_header {
  uint32_t magic;
  float volume_mult;
  float freq_mult;
  uint32_t unknown[2];
  uint32_t vel_region_count;
  ibnk_inst_instnew_vel_region vel_regions[0];

  void byteswap() {
    this->volume_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->volume_mult));
    this->freq_mult = bswap32f(*reinterpret_cast<uint32_t*>(&this->freq_mult));
    this->vel_region_count = bswap32(this->vel_region_count);
    for (size_t x = 0; x < this->vel_region_count; x++) {
      this->vel_regions[x].byteswap();
    }
  }
};

struct ibnk_bank_header {
  uint32_t magic; // 'BANK'
  uint32_t inst_offsets[245];

  void byteswap() {
    for (size_t x = 0; x < 245; x++) {
      this->inst_offsets[x] = bswap32(this->inst_offsets[x]);
    }
  }
};

struct ibnk_list_header {
  uint32_t magic; // 'LIST'
  uint32_t size;
  uint32_t count;
  uint32_t inst_offsets[0];

  void byteswap() {
    this->size = bswap32(this->size);
    this->count = bswap32(this->count);
    for (size_t x = 0; x < count; x++) {
      this->inst_offsets[x] = bswap32(this->inst_offsets[x]);
    }
  }
};

struct ibnk_header {
  uint32_t magic; // 'IBNK'
  uint32_t size;
  uint32_t bank_id;
  uint32_t unknown1[5];

  void byteswap() {
    this->size = bswap32(this->size);
    this->bank_id = bswap32(this->bank_id);
  }
};

struct ibnk_chunk_header {
  uint32_t magic;
  uint32_t size;

  void byteswap() {
    this->size = bswap32(this->size);
  }
};

Instrument ibnk_inst_decode(void* vdata, size_t offset, size_t inst_id) {
  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);
  uint8_t* inst_data = data + offset;
  Instrument result_inst(inst_id);

  // old-style instrument (Luigi's Mansion / Pikmin era)
  if (!memcmp(inst_data, "INST", 4)) {
    ibnk_inst_inst_header* inst = reinterpret_cast<ibnk_inst_inst_header*>(inst_data);
    inst->byteswap();

    if (inst->freq_mult == 0) {
      inst->freq_mult = 1;
    }
    if (inst->volume_mult == 0) {
      inst->volume_mult = 1;
    }

    uint8_t key_low = 0;
    for (uint32_t x = 0; x < inst->key_region_count; x++) {
      ibnk_inst_inst_key_region* key_region = reinterpret_cast<ibnk_inst_inst_key_region*>(
          data + inst->key_region_offsets[x]);
      key_region->byteswap();

      result_inst.key_regions.emplace_back(key_low, key_region->key_high);
      auto& result_key_region = result_inst.key_regions.back();

      uint8_t vel_low = 0;
      for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
        ibnk_inst_inst_vel_region* vel_region = reinterpret_cast<ibnk_inst_inst_vel_region*>(
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
    return result_inst;
  }

  // new-style Perc instruments (Twilight Princess)
  if (!memcmp(inst_data, "Perc", 4)) {
    ibnk_inst_percnew_header* percnew_header = reinterpret_cast<ibnk_inst_percnew_header*>(inst_data);
    percnew_header->byteswap();

    for (size_t z = 0; z < percnew_header->count; z++) {
      if (!percnew_header->pmap_offsets[z]) {
        continue;
      }

      ibnk_inst_pmap_header* pmap_header = reinterpret_cast<ibnk_inst_pmap_header*>(
          data + percnew_header->pmap_offsets[z]);
      pmap_header->byteswap();

      result_inst.key_regions.emplace_back(z, z);
      auto& result_key_region = result_inst.key_regions.back();

      uint8_t vel_low = 0;
      for (uint32_t y = 0; y < pmap_header->vel_region_count; y++) {
        // TODO: should we multiply by the pmap's freq_mult here? there's a hack
        // for old-style instruments where we don't use the INST's freq_mult;
        // figure out if we should do the same here (currently we don't)
        auto& vel_region = pmap_header->vel_regions[y];
        result_key_region.vel_regions.emplace_back(vel_low,
            vel_region.vel_high, vel_region.sample_bank_id,
            vel_region.sample_num, vel_region.freq_mult * pmap_header->freq_mult,
            vel_region.volume_mult * pmap_header->volume_mult);
        vel_low = vel_region.vel_high + 1;
      }
    }

    return result_inst;
  }

  // new-style Inst instruments (Twilight Princess)
  if (!memcmp(inst_data, "Inst", 4)) {
    ibnk_inst_instnew_header* instnew_header = reinterpret_cast<ibnk_inst_instnew_header*>(inst_data);
    instnew_header->byteswap();

    result_inst.id = instnew_header->inst_id;

    if (instnew_header->key_region_count > 0x7F) {
      throw runtime_error("key region count is too large");
    }

    // sigh... why did they specify these structs inline and use offsets
    // everywhere else? just for maximum tedium? we'll reuse offset to keep
    // track of what we've already parsed
    offset += sizeof(ibnk_inst_instnew_header);
    uint8_t key_low = 0;
    for (size_t z = 0; z < instnew_header->key_region_count; z++) {
      ibnk_inst_instnew_key_region* key_region = reinterpret_cast<ibnk_inst_instnew_key_region*>(
          data + offset);
      key_region->byteswap();
      offset += sizeof(ibnk_inst_instnew_key_region);

      result_inst.key_regions.emplace_back(key_low, key_region->key_high);
      auto& result_key_region = result_inst.key_regions.back();

      uint8_t vel_low = 0;
      for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
        ibnk_inst_instnew_vel_region* vel_region = reinterpret_cast<ibnk_inst_instnew_vel_region*>(
            data + offset);
        vel_region->byteswap();
        offset += sizeof(ibnk_inst_instnew_vel_region);

        result_key_region.vel_regions.emplace_back(vel_low,
            vel_region->vel_high, vel_region->sample_bank_id,
            vel_region->sample_num, vel_region->freq_mult,
            vel_region->volume_mult);
        vel_low = vel_region->vel_high + 1;
      }
      key_low = key_region->key_high + 1;
    }

    // after all that, there's an instrument-global volume and freq mult. go
    // through all the vel regions and apply these factors appropriately
    ibnk_inst_instnew_footer* footer = reinterpret_cast<ibnk_inst_instnew_footer*>(
        data + offset);
    footer->byteswap();
    for (auto& key_region : result_inst.key_regions) {
      for (auto& vel_region : key_region.vel_regions) {
        vel_region.volume_mult *= footer->volume_mult;
        vel_region.freq_mult *= footer->freq_mult;
      }
    }

    return result_inst;
  }

  // old-style PERC and PER2 instruments (Luigi's Mansion / Pikmin era)
  uint32_t* offset_table = NULL;
  uint32_t count = 0;
  if (!memcmp(inst_data, "PERC", 4)) {
    ibnk_inst_perc_header* perc = reinterpret_cast<ibnk_inst_perc_header*>(inst_data);
    perc->byteswap();
    offset_table = perc->key_region_offsets;
    count = 0x7F;

  } else if (!memcmp(inst_data, "PER2", 4)) {
    ibnk_inst_per2_header* per2 = reinterpret_cast<ibnk_inst_per2_header*>(inst_data);
    per2->byteswap();
    offset_table = per2->key_region_offsets;
    count = 0x64;

  } else {
    throw invalid_argument(string_printf("unknown instrument format at %08zX: %.4s (%08X)",
        offset, inst_data, *reinterpret_cast<const uint32_t*>(inst_data)));
  }

  for (uint32_t x = 0; x < count; x++) {
    if (!offset_table[x]) {
      continue;
    }

    ibnk_inst_per2_key_region* key_region = reinterpret_cast<ibnk_inst_per2_key_region*>(
        data + offset_table[x]);
    key_region->byteswap();

    result_inst.key_regions.emplace_back(x, x);
    auto& result_key_region = result_inst.key_regions.back();

    uint8_t vel_low = 0;
    for (uint32_t y = 0; y < key_region->vel_region_count; y++) {
      ibnk_inst_inst_vel_region* vel_region = reinterpret_cast<ibnk_inst_inst_vel_region*>(
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
  return result_inst;
}

InstrumentBank ibnk_decode(void* vdata) {
  if (memcmp(vdata, "IBNK", 4)) {
    throw invalid_argument("IBNK file not at expected offset");
  }

  ibnk_header* ibnk = reinterpret_cast<ibnk_header*>(vdata);
  ibnk->byteswap();

  InstrumentBank result_bank(ibnk->bank_id);

  // for older games, the BANK chunk immediately follows the IBNK header. for
  // newer games, there's no BANK chunk at all.
  size_t offset = sizeof(ibnk_header);
  {
    ibnk_chunk_header* first_chunk_header = reinterpret_cast<ibnk_chunk_header*>(
        reinterpret_cast<char*>(vdata) + offset);
    if (!memcmp(&first_chunk_header->magic, "BANK", 4)) {
      ibnk_bank_header* bank_header = reinterpret_cast<ibnk_bank_header*>(
          reinterpret_cast<char*>(vdata) + offset);
      bank_header->byteswap();
      for (size_t z = 0; z < 245; z++) {
        if (!bank_header->inst_offsets[z]) {
          continue;
        }
        try {
          auto inst = ibnk_inst_decode(vdata, bank_header->inst_offsets[z], z);
          result_bank.id_to_instrument.emplace(z, inst);
        } catch (const exception& e) {
          fprintf(stderr, "warning: failed to decode instrument: %s\n", e.what());
        }
      }
      return result_bank;
    }
  }

  while (offset < ibnk->size) {
    ibnk_chunk_header* chunk_header = reinterpret_cast<ibnk_chunk_header*>(
        reinterpret_cast<char*>(vdata) + offset);

    // note: we skip INST even though it contains relevant data because the LIST
    // chunk countains references to it and we parse it through there instead
    if (!memcmp(&chunk_header->magic, "ENVT", 4) ||
        !memcmp(&chunk_header->magic, "OSCT", 4) ||
        !memcmp(&chunk_header->magic, "PMAP", 4) ||
        !memcmp(&chunk_header->magic, "PERC", 4) ||
        !memcmp(&chunk_header->magic, "RAND", 4) ||
        !memcmp(&chunk_header->magic, "SENS", 4) ||
        !memcmp(&chunk_header->magic, "INST", 4)) {
      // sometimes these chunks aren't aligned to 4-byte boundaries, but all
      // chunk headers are aligned. looks like they just force alignment in the
      // file, so do that here too
      chunk_header->byteswap();
      offset = (offset + sizeof(chunk_header) + chunk_header->size + 3) & (~3);

    // there might be a few zeroes to pad out the IBNK block at the end (looks
    // like they want to be aligned to 0x20-byte boundaries?)
    } else if (chunk_header->magic == 0) {
      offset += 4;

    } else if (!memcmp(&chunk_header->magic, "LIST", 4)) {
      ibnk_list_header* list_header = reinterpret_cast<ibnk_list_header*>(
          reinterpret_cast<char*>(vdata) + offset);
      list_header->byteswap();
      for (size_t z = 0; z < list_header->count; z++) {
        if (!list_header->inst_offsets[z]) {
          continue;
        }
        try {
          auto inst = ibnk_inst_decode(vdata, list_header->inst_offsets[z], z);
          result_bank.id_to_instrument.emplace(z, inst);
        } catch (const exception& e) {
          fprintf(stderr, "warning: failed to decode instrument: %s\n", e.what());
        }
      }
      offset += list_header->size + sizeof(ibnk_list_header);

    } else if (!memcmp(&chunk_header->magic, "BANK", 4)) {
      throw runtime_error(string_printf("IBNK contains BANK at %08zX but it is not first",
          offset));

    } else {
      throw runtime_error(string_printf("unknown IBNK chunk type at %08zX: %.4s",
          offset, reinterpret_cast<const char*>(&chunk_header->magic)));
    }
  }

  return result_bank;
}
