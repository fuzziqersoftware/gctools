#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <phosg-audio/File.hh>
#include <vector>

#include "aaf.hh"

#ifdef WINDOWS
#define PRIu32 "u"
#define PRIX32 "X"
#define PRIX64 "llX"
#endif

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
    fprintf(stderr, "usage: %s bank_directory output_directory\n", argv[0]);
    return 1;
  }

  auto env = load_sound_environment(argv[1]);

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

  for (const auto& wsys_it : env.sample_banks) {
    for (const auto& s : wsys_it.second) {
      auto samples = s.samples();
      if (samples.empty()) {
        fprintf(stderr, "warning: can\'t decode %s:%" PRIX32 ":%" PRIX32 "\n",
            s.source_filename.c_str(), s.source_offset, s.source_size);
        continue;
      }
      string filename = string_printf("%s/sample-%s-%" PRIX64 "-%08" PRIX32
          "-%08" PRIX32 "-%08" PRIX32 ".wav", argv[2],
          s.source_filename.c_str(), s.source_offset, s.sound_id,
          s.aw_file_index, s.wave_table_index);
      save_wav(filename.c_str(), samples, s.sample_rate, s.num_channels);
    }
  }

  for (const auto& s : env.sequence_programs) {
    string fn = string_printf("%s/sequence-%" PRIu32 "-%s.bms", argv[2],
        s.second.index, s.first.c_str());
    save_file(fn, s.second.data);
  }

  return 0;
}
