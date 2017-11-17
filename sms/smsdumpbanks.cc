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

#include "../wav.hh"
#include "aaf.hh"

using namespace std;



int main(int argc, char** argv) {
  if (argc != 3) {
    throw invalid_argument("bank directory and output directory required");
  }

  auto env = aaf_decode_directory(argv[1]);

  for (const auto& ibank_it : env.instrument_banks) {
    const auto& ibank = ibank_it.second;

    string filename = string_printf("%s/bank-%" PRIu32 ".repr", argv[2],
        ibank_it.first);
    auto f = fopen_unique(filename.c_str(), "wt");

    fprintf(f.get(), "{\n");
    for (const auto& inst_it : ibank.id_to_instrument) {
      fprintf(f.get(), "  0x%" PRIX32 ": [\n", inst_it.first);
      for (const auto& key_region : inst_it.second.key_regions) {
        fprintf(f.get(), "    [0x%02hhX, 0x%02hhX,\n", key_region.key_low,
            key_region.key_high);
        for (const auto& vel_region : key_region.vel_regions) {
          fprintf(f.get(), "      [0x%02hhX, 0x%02hhX, 0x%hX, %g],\n",
              vel_region.vel_low, vel_region.vel_high, vel_region.sound_id,
              vel_region.freq_mult);
        }
        fprintf(f.get(), "    ],\n");
      }
      fprintf(f.get(), "  ],\n");
    }
    fprintf(f.get(), "}");
  }

  for (const auto& wsys : env.sample_banks) {
    for (const auto& s : wsys) {
      string filename = string_printf("%s/sample-%s-%" PRIX64 "-%08" PRIX32
          "-%08" PRIX32 ".wav", argv[2], s.source_filename.c_str(), s.sound_id,
          s.aw_file_index, s.wave_table_index);
      save_wav(filename.c_str(), s.samples, s.sample_rate, s.num_channels);
    }
  }

  return 0;
}
