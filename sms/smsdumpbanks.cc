#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>

#include "wav.hh"
#include "aaf.hh"

using namespace std;



string name_for_note(uint8_t note) {
  if (note >= 0x80) {
    return "invalid-note";
  }
  const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
      "A#", "B"};
  return string_printf("%s%hhu", names[note % 12], note / 12);
}



int main(int argc, char** argv) {
  if (argc != 3) {
    throw invalid_argument("bank directory and output directory required");
  }

  auto env = aaf_decode_directory(argv[1]);

  for (const auto& ibank_it : env.instrument_banks) {
    const auto& ibank = ibank_it.second;

    string filename = string_printf("%s/bank-%" PRIu32 ".txt", argv[2],
        ibank_it.first);
    auto f = fopen_unique(filename.c_str(), "wt");

    for (const auto& inst_it : ibank.id_to_instrument) {
      fprintf(f.get(), "instrument %" PRIu32 " (0x%" PRIX32 "):\n",
          inst_it.first, inst_it.first);
      for (const auto& key_region : inst_it.second.key_regions) {
        string key_low_str = name_for_note(key_region.key_low);
        string key_high_str = name_for_note(key_region.key_high);
        fprintf(f.get(), "  key region [%hhd,%hhd] / [0x%02hhX,0x%02hhX] / [%s,%s]:\n",
            key_region.key_low, key_region.key_high, key_region.key_low,
            key_region.key_high, key_low_str.c_str(), key_high_str.c_str());
        for (const auto& vel_region : key_region.vel_regions) {
          if (vel_region.sound) {
            fprintf(f.get(), "    velocity region [%hhd,%hhd] / [0x%02hhX,0x%02hhX]: sound id 0x%hX, frequency multiplier %g, base note %02hhX, sound base note %02hhX\n",
                vel_region.vel_low, vel_region.vel_high, vel_region.vel_low,
                vel_region.vel_high, vel_region.sound_id, vel_region.freq_mult,
                vel_region.base_note, vel_region.sound->base_note);
          } else {
            fprintf(f.get(), "    velocity region [%hhd,%hhd] / [0x%02hhX,0x%02hhX]: sound id 0x%hX, frequency multiplier %g, base note %02hhX, sound base note missing\n",
                vel_region.vel_low, vel_region.vel_high, vel_region.vel_low,
                vel_region.vel_high, vel_region.sound_id, vel_region.freq_mult,
                vel_region.base_note);
          }
        }
      }
    }
  }

  for (const auto& wsys : env.sample_banks) {
    for (const auto& s : wsys) {
      string filename = string_printf("%s/sample-%s-%" PRIX64 "-%08" PRIX32
          "-%08" PRIX32 ".wav", argv[2], s.source_filename.c_str(), s.sound_id,
          s.aw_file_index, s.wave_table_index);
      save_wav(filename.c_str(), s.samples(), s.sample_rate, s.num_channels);
    }
  }

  for (const auto& s : env.sequence_programs) {
    string fn = string_printf("%s/sequence-%s.bms", argv[2], s.first.c_str());
    save_file(fn, s.second);
  }

  return 0;
}
