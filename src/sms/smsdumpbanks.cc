#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <phosg-audio/File.hh>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
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
  return string_printf("%s%hhu", names[note % 12], static_cast<uint8_t>(note / 12));
}

string base_filename_for_sound(const Sound& s) {
  return string_printf("sample-%s-%" PRIX32 "-%08" PRIX64 "-%08" PRIX32
                       "-%08" PRIX32,
      s.source_filename.c_str(), s.source_offset,
      s.sound_id, s.aw_file_index, s.wave_table_index);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s bank_directory output_directory\n", argv[0]);
    return 1;
  }

  auto env = load_sound_environment(argv[1]);

  // generate text file
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

  // generate soundfont text file
  {
    string filename = string_printf("%s/metadata-sf.txt", argv[2]);
    auto f = fopen_unique(filename.c_str(), "wt");

    map<string, bool> filenames;

    fprintf(f.get(), "[Samples]\n\n");
    for (const auto& bank_it : env.sample_banks) {
      for (const Sound& s : bank_it.second) {
        string sound_basename = base_filename_for_sound(s);
        fprintf(f.get(), "\
    SampleName=%s.wav\n\
        SampleRate=%zu\n\
        Key=%hhu\n\
        FineTune=0\n\
        Type=1\n\n",
            sound_basename.c_str(), s.sample_rate, s.base_note);
        filenames.emplace(sound_basename, false);
      }
    }

    fprintf(f.get(), "[Instruments]\n\n");
    for (const auto& ibank_it : env.instrument_banks) {
      const auto& ibank = ibank_it.second;
      for (const auto& inst_it : ibank.id_to_instrument) {
        string instrument_name = string_printf("inst_%08" PRIX32 "_%08" PRIX32, ibank.id, inst_it.first);
        fprintf(f.get(), "    InstrumentName=%s\n\n", instrument_name.c_str());
        for (const auto& key_region : inst_it.second.key_regions) {
          for (const auto& vel_region : key_region.vel_regions) {
            if (!vel_region.sound) {
              fprintf(stderr, "warning: sound missing for instrument=%08" PRIX32 ":%08" PRIX32 " key=[%hhu,%hhu] vel=[%hhd,%hhd]: sound id 0x%hX, frequency multiplier %g, base note %02hhX\n",
                  ibank.id, inst_it.first, key_region.key_low, key_region.key_high,
                  vel_region.vel_low, vel_region.vel_high, vel_region.sound_id,
                  vel_region.freq_mult, vel_region.base_note);
            } else {
              string basename = base_filename_for_sound(*vel_region.sound);
              uint8_t base_note = vel_region.base_note ? vel_region.base_note : vel_region.sound->base_note;
              fprintf(f.get(), "\
        Sample=%s\n\
            Z_LowKey=%hhu\n\
            Z_HighKey=%hhu\n\
            Z_LowVelocity=%hhu\n\
            Z_HighVelocity=%hhu\n\
            Z_sampleModes=1\n\
            Z_overridingRootKey=%hhu\n\
            Z_Modulator=(NoteOnVelocity,ReverseDirection,Unipolar,Linear), initialFilterFc, 0, (NoteOnVelocity,ReverseDirection,Unipolar,Switch), 0\n\n",
                  basename.c_str(), key_region.key_low, key_region.key_high,
                  vel_region.vel_low, vel_region.vel_high, base_note);
              filenames[basename] = true;
            }
          }
        }
      }
    }

    fprintf(f.get(), "[Presets]\n\n");
    for (const auto& ibank_it : env.instrument_banks) {
      const auto& ibank = ibank_it.second;
      for (const auto& inst_it : ibank.id_to_instrument) {
        string instrument_name = string_printf("inst_%08" PRIX32 "_%08" PRIX32, ibank.id, inst_it.first);
        fprintf(f.get(), "\
    PresetName=preset_%s\n\
        Bank=%" PRIu32 "\n\
        Program=%" PRIu32 "\n\
\n\
        Instrument=%s\n\
            L_LowKey=0\n\
            L_HighKey=127\n\
            L_LowVelocity=0\n\
            L_HighVelocity=127\n\n",
            instrument_name.c_str(), ibank.id, inst_it.first, instrument_name.c_str());
      }
    }

    fprintf(f.get(), "\
[Info]\n\
Version=2.1\n\
Engine=\n\
Name=\n\
ROMName=\n\
ROMVersion=\n\
Date=\n\
Designer=\n\
Product=\n\
Copyright=\n\
Editor=\n\
Comments=\n");

    size_t num_unused = 0;
    for (const auto& it : filenames) {
      fprintf(stderr, "[check] %s %s.wav\n", it.second ? "used" : "UNUSED", it.first.c_str());
      if (!it.second) {
        num_unused++;
      }
    }
    fprintf(stderr, "[check] %zu/%zu unused\n", num_unused, filenames.size());
  }

  // export samples
  for (const auto& wsys_it : env.sample_banks) {
    for (const auto& s : wsys_it.second) {
      auto samples = s.samples();
      if (samples.empty()) {
        fprintf(stderr, "warning: can\'t decode %s:%" PRIX32 ":%" PRIX32 "\n",
            s.source_filename.c_str(), s.source_offset, s.source_size);
        continue;
      }
      string basename = base_filename_for_sound(s);
      string filename = string_printf("%s/%s.wav", argv[2], basename.c_str());
      save_wav(filename.c_str(), samples, s.sample_rate, s.num_channels);
    }
  }

  // export sequences
  for (const auto& s : env.sequence_programs) {
    string fn = string_printf("%s/sequence-%" PRIu32 "-%s.bms", argv[2],
        s.second.index, s.first.c_str());
    save_file(fn, s.second.data);
  }

  return 0;
}
