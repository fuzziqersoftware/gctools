#include <inttypes.h>
#include <stdio.h>

#include <array>
#include <deque>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <phosg-audio/File.hh>
#include <phosg-audio/Stream.hh>
#include <string>
#include <unordered_map>

#include "SampleCache.hh"

using namespace std;



enum Flags {
  TerminalColor       = 0x01,
  ShowSampleData      = 0x02,
  ShowSampleWaveforms = 0x04,
  Default             = 0x00,
};

struct Module {
  struct Instrument {
    string name;
    uint16_t num_samples;
    int8_t finetune; // Pitch shift (positive or negative) in increments of 1/8 semitone
    uint8_t volume; // 0-64
    uint16_t loop_start_samples;
    uint16_t loop_length_samples;
    string original_sample_data;
    vector<float> sample_data;

    bool loop_valid() const {
      return (this->loop_start_samples < this->sample_data.size()) &&
          ((this->loop_start_samples + this->loop_length_samples) < this->sample_data.size());
    }
  };

  struct Pattern {
    struct Division {
      uint16_t wx;
      uint16_t yz;

      uint8_t instrument_num() const {
        return ((this->wx >> 8) & 0xF0) | ((this->yz >> 12) & 0x0F);
      }
      uint16_t period() const {
        return this->wx & 0x0FFF;
      }
      uint16_t effect() const {
        return this->yz & 0x0FFF;
      }
    };

    vector<Division> divisions;
  };

  string name;
  size_t num_tracks;
  // Note: instrument references are 1-based pretty much everywhere and this
  // array is of course 0-based, so remember to +1/-1 the index as needed
  vector<Instrument> instruments;
  uint8_t partition_count;
  array<uint8_t, 0x80> partition_table;
  uint32_t extension_signature;
  vector<Pattern> patterns;
};



Module load_mod(StringReader& r) {
  Module mod;

  // First, look ahead to see if this file uses any extensions. Annoyingly, the
  // signature field is pretty late in the file format, and some preceding
  // fields' sizes depend on the enabled extensions.
  try {
    mod.extension_signature = r.pget_u32r(0x438);
  } catch (const out_of_range&) {
    mod.extension_signature = 0;
  }

  size_t num_instruments = 31; // This is only not 31 in the default case below
  switch (mod.extension_signature) {
    case 0x4D2E4B2E: // M.K.
    case 0x4D214B21: // M!K!
    case 0x464C5434: // FLT4
      mod.num_tracks = 4;
      break;
    case 0x464C5438: // FLT8
      mod.num_tracks = 8;
      break;
    default:
      if ((mod.extension_signature & 0xF0FFFFFF) == 0x3043484E) { // xCHN
        mod.num_tracks = (mod.extension_signature >> 24) & 0x0F;
      } else if ((mod.extension_signature & 0xF0F0FFFF) == 0x30304348) { // xxCH
        mod.num_tracks = (((mod.extension_signature >> 24) & 0x0F) * 10) +
            ((mod.extension_signature >> 16) & 0x0F);
      } else { // Unrecognized signature; probably a very old MOD
        num_instruments = 15;
        mod.num_tracks = 4;
      }
  }

  mod.name = r.read(0x14);
  strip_trailing_zeroes(mod.name);

  mod.instruments.resize(num_instruments);
  for (size_t x = 0; x < num_instruments; x++) {
    auto& i = mod.instruments[x];
    i.name = r.read(0x16);
    strip_trailing_zeroes(i.name);
    i.num_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
    i.finetune = r.get_u8();
    // Sign-extend finetune from 4->8 bits
    if (i.finetune & 0x08) {
      i.finetune |= 0xF0;
    } else {
      i.finetune &= 0x0F;
    }
    i.volume = r.get_u8();
    i.loop_start_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
    i.loop_length_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
  }

  mod.partition_count = r.get_u8();
  r.get_u8(); // unused
  r.read_into(mod.partition_table.data(), mod.partition_table.size());

  // We should have gotten to exactly the same offset that we read ahead to at
  // the beginning, unless there were not 31 instruments.
  uint32_t inplace_extension_signature = r.get_u32r();
  if (mod.extension_signature &&
      mod.extension_signature != inplace_extension_signature) {
    throw logic_error("read-ahead extension signature does not match inplace extension signature");
  }

  // Compute the number of patterns based on the contents of the partition
  // table. The number of patterns is the maximum value in the table (+1, since
  // pattern 0 is valid), and even patterns that do not appear in this table but
  // are less than the maximum value will exist in the file.
  size_t num_patterns = 0;
  for (size_t x = 0; x < mod.partition_count; x++) {
    if (num_patterns <= mod.partition_table[x]) {
      num_patterns = mod.partition_table[x] + 1;
    }
  }

  // Load the patterns.
  mod.patterns.resize(num_patterns);
  for (size_t x = 0; x < num_patterns; x++) {
    auto& pat = mod.patterns[x];
    pat.divisions.resize(mod.num_tracks * 64);
    for (auto& div : pat.divisions) {
      div.wx = r.get_u16r();
      div.yz = r.get_u16r();
    }
  }

  // Load the sample data for each instrument.
  for (auto& i : mod.instruments) {
    i.original_sample_data = r.read(i.num_samples);
    i.sample_data.resize(i.num_samples);
    for (size_t x = 0; x < i.num_samples; x++) {
      int8_t sample = i.original_sample_data[x];
      i.sample_data[x] = (sample == -0x80)
          ? -1.0
          : (static_cast<float>(sample) / 255.0f);
    }
  }

  return mod;
}



void disassemble_pattern_row(
    FILE* stream,
    const Module& mod,
    uint8_t pattern_num,
    uint8_t y,
    uint64_t flags) {
  static const unordered_map<uint16_t, const char*> note_name_for_period({
    {1712, "C 0"}, {1616, "C#0"}, {1525, "D 0"}, {1440, "D#0"}, {1357, "E 0"}, {1281, "F 0"}, {1209, "F#0"}, {1141, "G 0"}, {1077, "G#0"}, {1017, "A 0"}, {961,  "A#0"}, {907,  "B 0"},
    {856,  "C 1"}, {808,  "C#1"}, {762,  "D 1"}, {720,  "D#1"}, {678,  "E 1"}, {640,  "F 1"}, {604,  "F#1"}, {570,  "G 1"}, {538,  "G#1"}, {508,  "A 1"}, {480,  "A#1"}, {453,  "B 1"},
    {428,  "C 2"}, {404,  "C#2"}, {381,  "D 2"}, {360,  "D#2"}, {339,  "E 2"}, {320,  "F 2"}, {302,  "F#2"}, {285,  "G 2"}, {269,  "G#2"}, {254,  "A 2"}, {240,  "A#2"}, {226,  "B 2"},
    {214,  "C 3"}, {202,  "C#3"}, {190,  "D 3"}, {180,  "D#3"}, {170,  "E 3"}, {160,  "F 3"}, {151,  "F#3"}, {143,  "G 3"}, {135,  "G#3"}, {127,  "A 3"}, {120,  "A#3"}, {113,  "B 3"},
    {107,  "C 4"}, {101,  "C#4"}, {95,   "D 4"}, {90,   "D#4"}, {85,   "E 4"}, {80,   "F 4"}, {76,   "F#4"}, {71,   "G 4"}, {67,   "G#4"}, {64,   "A 4"}, {60,   "A#4"}, {57,   "B 4"},
    {0,    "---"},
  });

  static const TerminalFormat track_colors[6] = {
    TerminalFormat::FG_RED,
    TerminalFormat::FG_CYAN,
    TerminalFormat::FG_YELLOW,
    TerminalFormat::FG_GREEN,
    TerminalFormat::FG_MAGENTA,
    TerminalFormat::FG_WHITE,
  };
  bool use_color = flags & Flags::TerminalColor;

  const auto& p = mod.patterns.at(pattern_num);
  fputs("  ", stream);
  if (use_color) {
    if (y == 0) {
      print_color_escape(stream, TerminalFormat::FG_WHITE, TerminalFormat::BOLD, TerminalFormat::INVERSE, TerminalFormat::END);
    } else {
      print_color_escape(stream, TerminalFormat::FG_WHITE, TerminalFormat::END);
    }
  }
  fprintf(stream, "%02hhu +%2hhu", pattern_num, y);
  for (size_t z = 0; z < mod.num_tracks; z++) {
    const auto& div = p.divisions[y * mod.num_tracks + z];
    uint8_t instrument_num = div.instrument_num();
    uint16_t period = div.period();
    uint16_t effect = div.effect();

    if (!instrument_num && !period && !effect) {
      if (use_color) {
        print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
      }
      fputs("  =            ", stream);
    } else {
      if (use_color) {
        print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
      }
      fputs("  =", stream);
      if (use_color) {
        if (instrument_num || period) {
          print_color_escape(stream, track_colors[z % 6], TerminalFormat::BOLD, TerminalFormat::END);
        } else {
          print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
        }
      }

      if (instrument_num) {
        fprintf(stream, "  %02hu", instrument_num);
      } else {
        fprintf(stream, "  --");
      }
      try {
        fprintf(stream, " %s", note_name_for_period.at(period));
      } catch (const out_of_range&) {
        fprintf(stream, " %03hX", period);
      }
      if (effect) {
        fprintf(stream, " %03hX", effect);
      } else {
        fprintf(stream, " ---");
      }
    }
  }
  if (use_color) {
    print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
  }
}

void disassemble_mod(FILE* stream, const Module& mod, uint64_t flags) {
  fprintf(stream, "Name: %s\n", mod.name.c_str());
  fprintf(stream, "Tracks: %zu\n", mod.num_tracks);
  fprintf(stream, "Instruments: %zu\n", mod.instruments.size());
  fprintf(stream, "Partitions: %hhu\n", mod.partition_count);
  fprintf(stream, "Extension signature: %08X\n", mod.extension_signature);

  for (size_t x = 0; x < mod.instruments.size(); x++) {
    const auto& i = mod.instruments[x];
    fputc('\n', stream);
    string escaped_name = escape_quotes(i.name);
    fprintf(stream, "Instrument %zu: %s\n", x + 1, escaped_name.c_str());
    fprintf(stream, "  Fine-tune: %c%d/8 semitones\n",
        (i.finetune < 0) ? '-' : '+', (i.finetune < 0) ? -i.finetune : i.finetune);
    fprintf(stream, "  Volume: %hhu/64\n", i.volume);
    fprintf(stream, "  Loop: start at %hu for %hu samples\n", i.loop_start_samples, i.loop_length_samples);
    fprintf(stream, "  Data: (%zu samples)\n", i.sample_data.size());

    if (flags & Flags::ShowSampleData) {
      print_data(stream, i.original_sample_data);
    }

    if (flags & Flags::ShowSampleWaveforms) {
      string line_data(0x80, ' ');
      for (size_t z = 0; z < i.original_sample_data.size(); z++) {
        const char* suffix = "";
        if (z == i.loop_start_samples) {
          suffix = "LOOP START";
        } else if (z == i.loop_start_samples + i.loop_length_samples) {
          suffix = "LOOP END";
        }
        int8_t sample = i.original_sample_data[z];
        uint16_t offset = (static_cast<int16_t>(sample) + 0x80) / 2;
        line_data[offset] = '*';
        if (((sample == -0x80) || (sample == 0x7F)) && (flags & Flags::TerminalColor)) {
          print_color_escape(stream, TerminalFormat::FG_RED, TerminalFormat::BOLD, TerminalFormat::END);
        }
        fprintf(stream, "  ins %02zu +%04zX [%s]%s\n", x + 1, z, line_data.c_str(), suffix);
        if (((sample == -0x80) || (sample == 0x7F)) && (flags & Flags::TerminalColor)) {
          print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
        }
        line_data[offset] = ' ';
      }
    }
  }

  for (size_t x = 0; x < mod.patterns.size(); x++) {
    fputc('\n', stream);
    fprintf(stream, "Pattern %zu\n", x);
    for (size_t y = 0; y < 64; y++) {
      disassemble_pattern_row(stream, mod, x, y, flags);
      fputc('\n', stream);
    }
  }

  fputc('\n', stream);
  fprintf(stream, "Partition table:\n");
  for (size_t x = 0; x < mod.partition_count; x++) {
    fprintf(stream, "  Partition %zu: %hu\n", x, mod.partition_table[x]);
  }
}



void export_mod_instruments(
    const Module& mod, size_t output_sample_rate, const char* output_prefix) {
  // Andrew's observational spec notes that about 8287 bytes of data are sent to
  // the channel when a normal sample is played at C2. Empirically, it seems
  // like this is 0.5x the sample rate we need to make music sound normal. I
  // don't (yet) know why this is.
  const float upscale_ratio = static_cast<double>(output_sample_rate) / 16574.0;
  for (size_t x = 0; x < mod.instruments.size(); x++) {
    const auto& i = mod.instruments[x];
    if (i.sample_data.empty()) {
      fprintf(stderr, "... (%zu) \"%s\" -> (no sound data)\n",
          x,
          i.name.c_str());
    } else {
      auto upscaled_data = resample(
          i.sample_data, 1, upscale_ratio, SRC_SINC_BEST_QUALITY);
      string output_filename = string_printf("%s_%zu.wav", output_prefix, x + 1);
      fprintf(stderr, "... (%zu) \"%s\" -> %zu samples, %zu upscaled, +%hhdft, %02hhX vol, loop [%hux%hu] => %s\n",
          x,
          i.name.c_str(),
          i.sample_data.size(),
          upscaled_data.size(),
          i.finetune,
          i.volume,
          i.loop_start_samples,
          i.loop_length_samples,
          output_filename.c_str());
      save_wav(output_filename.c_str(), upscaled_data, output_sample_rate, 1);
    }
  }
}



struct Timing {
  size_t sample_rate;
  size_t beats_per_minute;
  size_t ticks_per_division;
  double divisions_per_minute;
  double ticks_per_second;
  double samples_per_tick;

  // The observational specification by Andrew Scott gives this equation
  // describing how timing works:
  // divisions/minute = 24 * (beats/minute) / (ticks/division)
  // The number of samples per tick, then, is:
  // samples/tick = (samples/sec) * (secs/tick)
  // samples/tick = sample_rate / (ticks/sec)
  // samples/tick = sample_rate / (divisions/sec * ticks/division)
  // samples/tick = sample_rate / ((divisions/min / 60) * ticks/division)
  // samples/tick = sample_rate * 60 / (divisions/min * ticks/division)
  Timing(
      size_t sample_rate,
      size_t beats_per_minute = 125,
      size_t ticks_per_division = 6)
    : sample_rate(sample_rate),
      beats_per_minute(beats_per_minute),
      ticks_per_division(ticks_per_division),
      divisions_per_minute(static_cast<double>(24 * this->beats_per_minute)
        / this->ticks_per_division),
      ticks_per_second(
        static_cast<double>(this->divisions_per_minute * this->ticks_per_division) / 60),
      samples_per_tick(static_cast<double>(this->sample_rate * 60)
        / (this->divisions_per_minute * this->ticks_per_division)) { }
  string str() const {
    return string_printf("%zukHz %zubpm %zut/d => %lgd/m %lgt/sec %lgsmp/t",
        this->sample_rate,
        this->beats_per_minute,
        this->ticks_per_division,
        this->divisions_per_minute,
        this->ticks_per_second,
        this->samples_per_tick);
  }
};

struct TrackState {
  int32_t instrument_num; // 1-based! 0 = no instrument
  int32_t period;
  int32_t volume;
  double input_sample_offset; // relative to input samples, not any resampling thereof
  uint8_t vibrato_waveform;
  float vibrato_offset;

  uint8_t arpeggio_arg;
  uint8_t sample_retrigger_interval_ticks;
  uint8_t sample_start_delay_ticks;
  int16_t per_tick_period_increment;
  int16_t per_tick_volume_increment;
  int16_t slide_target_period;
  int16_t vibrato_amplitude;
  int16_t vibrato_cycles;
  // These are not reset each division, and are used for effects that continue a
  // previous effect
  int16_t last_slide_target_period;
  int16_t last_per_tick_period_increment;
  int16_t last_vibrato_amplitude;
  int16_t last_vibrato_cycles;

  TrackState()
    : instrument_num(0),
      period(0),
      volume(64),
      input_sample_offset(0.0),
      vibrato_waveform(0),
      vibrato_offset(0.0),
      last_slide_target_period(0),
      last_per_tick_period_increment(0),
      last_vibrato_amplitude(0),
      last_vibrato_cycles (0) {
    this->reset_division_scoped_effects();
  }

  void reset_division_scoped_effects() {
    this->arpeggio_arg = 0;
    this->sample_retrigger_interval_ticks = 0;
    this->sample_start_delay_ticks = 0;
    this->per_tick_period_increment = 0;
    this->per_tick_volume_increment = 0;
    this->slide_target_period = 0;
    this->vibrato_amplitude = 0;
    this->vibrato_cycles = 0;
  }
};

void render_mod(
    const Module& mod,
    size_t output_sample_rate,
    int resample_method,
    size_t skip_partitions,
    uint64_t flags,
    std::function<void(vector<float>&&)> on_tick_complete) {
  SampleCache<uint8_t> sample_cache(resample_method);

  Timing timing(output_sample_rate);
  size_t current_partition = skip_partitions; // index in partition table
  ssize_t current_cell = 0; // division index in pattern (0-63)
  ssize_t pattern_loop_start = 0;
  ssize_t pattern_loop_times_remaining = -1;
  bool jump_to_pattern_loop_start = false;

  {
    string s = timing.str();
    fprintf(stderr, "render: initial timing is %s\n", s.c_str());
  }

  vector<TrackState> tracks(mod.num_tracks);
  size_t total_output_samples = 0;
  while (current_partition < mod.partition_count) {
    // Show current state
    uint8_t current_pattern = mod.partition_table.at(current_partition);
    fprintf(stderr, "  %02zu", current_partition);
    disassemble_pattern_row(stderr, mod, current_pattern, current_cell, flags);
    fprintf(stderr, "  @ %.07gs\n",
        static_cast<float>(total_output_samples) / (2 * output_sample_rate));

    // Execute all commands in the current division in each track
    int32_t pattern_break_target = -1;
    ssize_t divisions_to_delay = 0;
    const auto& pattern = mod.patterns.at(current_pattern);
    for (size_t track_index = 0; track_index < mod.num_tracks; track_index++) {
      auto& track = tracks[track_index];
      const auto& div = pattern.divisions.at(current_cell * mod.num_tracks + track_index);

      // If a new note is to be played, reset the instrument number and/or
      // period, and the sample data offset (it will play from the beginning).
      // Effects [3] and [5] are special cases and do not result in a new note
      // being played, since they use the period as an additional parameter.
      uint16_t effect = div.effect();
      if (((effect & 0xF00) != 0x300) && ((effect & 0xF00) != 0x500)) {
        if (div.instrument_num()) {
          track.instrument_num = div.instrument_num();
          track.input_sample_offset = 0.0;
          track.volume = 64;
          if (!(track.vibrato_waveform & 4)) {
            track.vibrato_offset = 0.0;
          }
        }
        if (div.period()) {
          track.period = div.period();
          track.input_sample_offset = 0.0;
          // It seems like volume should NOT get reset in this case. TODO: is
          // this actually true?
          if (!(track.vibrato_waveform & 4)) {
            track.vibrato_offset = 0.0;
          }
        }
      }

      switch (effect & 0xF00) {
        case 0x000: // Arpeggio (or no effect)
          track.arpeggio_arg = effect & 0x0FF;
          break;

        case 0x100: // Slide up
          track.per_tick_period_increment = -(effect & 0x0FF);
          break;
        case 0x200: // Slide down
          track.per_tick_period_increment = effect & 0x0FF;
          break;
        case 0x300: // Slide to note
          track.slide_target_period = div.period();
          if (track.slide_target_period == 0) {
            track.slide_target_period = track.last_slide_target_period;
          }

          track.per_tick_period_increment = effect & 0xFF;
          if (track.per_tick_period_increment == 0) {
            track.per_tick_period_increment = track.last_per_tick_period_increment;
          } else if (track.slide_target_period < track.period) {
            track.per_tick_period_increment = -track.per_tick_period_increment;
          }

          track.last_slide_target_period = track.slide_target_period;
          track.last_per_tick_period_increment = track.per_tick_period_increment;
          break;

        case 0x400: // Vibrato
          track.vibrato_amplitude = effect & 0x00F;
          if (!track.vibrato_amplitude) {
            track.vibrato_amplitude = track.last_vibrato_amplitude;
          } else {
            track.last_vibrato_amplitude = track.vibrato_amplitude;
          }
          track.vibrato_cycles = (effect & 0x0F0) >> 4;
          if (!track.vibrato_cycles) {
            track.vibrato_cycles = track.last_vibrato_cycles;
          } else {
            track.last_vibrato_cycles = track.vibrato_cycles;
          }
          break;

        case 0x500: // Volume slide during slide to note
          // If this division has a period, use it; otherwise use the last
          // target period. (TODO: is this the correct behavior? The
          // observational spec says to use the division's period, but I've so
          // far only seen this used with no period.)
          track.slide_target_period = div.period();
          if (!track.slide_target_period) {
            track.slide_target_period = track.last_slide_target_period;
          }
          track.per_tick_period_increment = track.last_per_tick_period_increment;
          goto VolumeSlideEffect;

        case 0x600: // Volume slide during vibrato
          track.vibrato_amplitude = track.last_vibrato_amplitude;
          track.vibrato_cycles = track.last_vibrato_cycles;
          goto VolumeSlideEffect;

/*
[7]: Tremolo
     Where [7][x][y] means "oscillate the sample volume using a
     particular waveform with amplitude y*(ticks - 1), such that (x *
     ticks)/64 cycles occur in the division". The waveform is set using
     effect [14][7]. Similar to [4].

[8]: -- Unused -- */

        case 0x900: // Set sample offset
          // The spec says the parameter is essentially <<8 but is measured in
          // words. This appears to be false - PlayerPRO shifts by 8 here (not
          // 9), and the MODs I've tried sound wrong when using 9.
          track.input_sample_offset = static_cast<int32_t>(effect & 0x0FF) << 8;
          break;

        VolumeSlideEffect:
        case 0xA00: // Volume slide
          if (effect & 0x0F0) {
            track.per_tick_volume_increment = (effect & 0x0F0) >> 4;
          } else {
            track.per_tick_volume_increment = -(effect & 0x00F);
          }
          break;

/*
[11]: Position Jump
     Where [11][x][y] means "stop the pattern after this division, and
     continue the song at song-position x*16+y". This shifts the
     'pattern-cursor' in the pattern table (see above). Legal values for
     x*16+y are from 0 to 127. */

        case 0xC00: // Set volume
          track.volume = effect & 0x0FF;
          break;
        case 0xD00: // Pattern break
          // This was probably just a typo in the original Protracker, but it's
          // now propagated everywhere... the high 4 bits are multiplied by 10,
          // not 16.
          pattern_break_target = (((effect & 0x0F0) >> 4) * 10) + (effect & 0x00F);
          break;
        case 0xE00: { // Sub-effects
          switch (effect & 0x0F0) {
            case 0x000: // Enable/disable hardware filter
              // This is a hardware command on some Amigas; it looks like
              // PlayerPRO doesn't implement it, so neither will we.
              break;

/*
[14][1]: Fineslide up
     Where [14][1][x] means "decrement the period of the current sample
     by x". The incrementing takes place at the beginning of the
     division, and hence there is no actual sliding. You cannot slide
     beyond the note B3 (period 113).

[14][2]: Fineslide down
     Where [14][2][x] means "increment the period of the current sample
     by x". Similar to [14][1] but shifts the pitch down. You cannot
     slide beyond the note C1 (period 856).

[14][3]: Set glissando on/off
     Where [14][3][x] means "set glissando ON if x is 1, OFF if x is 0".
     Used in conjunction with [3] ('Slide to note'). If glissando is on,
     then 'Slide to note' will slide in semitones, otherwise will
     perform the default smooth slide. */

            case 0x040: // Set vibrato waveform
              // Note: there are only 8 waveforms defined (at least in the MOD
              // spec) so we don't bother with bit 3
              track.vibrato_waveform = effect & 0x007;
              break;

/*
[14][5]: Set finetune value
     Where [14][5][x] means "sets the finetune value of the current
     sample to the signed nibble x". x has legal values of 0..15,
     corresponding to signed nibbles 0..7,-8..-1 (see start of text for
     more info on finetune values). */

            case 0x060: { // Loop pattern
              uint8_t times = effect & 0x00F;
              if (times == 0) {
                pattern_loop_start = current_cell;
              } else if (pattern_loop_times_remaining == -1) {
                pattern_loop_times_remaining = times - 1;
                jump_to_pattern_loop_start = true;
              } else if (pattern_loop_times_remaining > 0) {
                pattern_loop_times_remaining--;
                jump_to_pattern_loop_start = true;
              } else {
                pattern_loop_times_remaining = -1;
              }
              break;
            }

/*
[14][7]: Set tremolo waveform
     Where [14][7][x] means "set the waveform of succeeding 'tremolo'
     effects to wave #x". Similar to [14][4], but alters effect [7] -
     the 'tremolo' effect.

[14][8]: -- Unused -- */

            case 0x090: // Retrigger sample every x ticks
              track.sample_retrigger_interval_ticks = effect & 0x0F;
              break;
            case 0x0A0: // Fine volumne slide up
              track.volume += effect & 0x00F;
              if (track.volume > 64) {
                track.volume = 64;
              }
              break;
            case 0x0B0: // Fine volumne slide up
              track.volume -= effect & 0x00F;
              if (track.volume < 0) {
                track.volume = 0;
              }
              break;
/*
[14][12]: Cut sample
     Where [14][12][x] means "after the current sample has been played
     for x ticks in this division, its volume will be set to 0". This
     implies that if x is 0, then you will not hear any of the sample.
     If you wish to insert "silence" in a pattern, it is better to use a
     "silence"-sample (see above) due to the lack of proper support for
     this effect. */

            case 0x0D0: // Delay sample
              track.sample_start_delay_ticks = effect & 0x00F;
              break;
            case 0x0E0: // Delay pattern
              divisions_to_delay = effect & 0x00F;
              break;

/*
[14][15]: Invert loop
     Where [14][15][x] means "if x is greater than 0, then play the
     current sample's loop upside down at speed x". Each byte in the
     sample's loop will have its sign changed (negated). It will only
     work if the sample's loop (defined previously) is not too big. The
     speed is based on an internal table. */

            default:
              goto UnimplementedEffect;
          }
          break;
        }

        case 0xF00: { // set speed
          uint8_t v = effect & 0xFF;
          if (v <= 32) {
            timing = Timing(timing.sample_rate, timing.beats_per_minute, v);
          } else {
            timing = Timing(timing.sample_rate, v, timing.ticks_per_division);
          }
          break;
        }
        UnimplementedEffect:
        default:
          fprintf(stderr, "warning: (part %zu pat %hhu y %zd track %zu) unimplemented effect %03hX = [%hhu][%hhu][%hhu]\n",
              current_partition,
              current_pattern,
              current_cell,
              track_index,
              effect,
              static_cast<uint8_t>((effect >> 8) & 0x0F),
              static_cast<uint8_t>((effect >> 4) & 0x0F),
              static_cast<uint8_t>(effect & 0x0F));
      }
    }

    // Render sound for each tick within the division
    for (ssize_t delayed_division_id = -1; delayed_division_id < divisions_to_delay; delayed_division_id++) {
      for (size_t tick_num = 0; tick_num < timing.ticks_per_division; tick_num++) {
        vector<float> tick_samples(timing.samples_per_tick * 2);
        for (size_t track_index = 0; track_index < mod.num_tracks; track_index++) {
          auto& track = tracks[track_index];
          if (track.instrument_num == 0) {
            continue; // Track has not played any sound yet
          }
          const auto& i = mod.instruments.at(track.instrument_num - 1);
          if (track.input_sample_offset >= i.sample_data.size()) {
            continue; // Previous sound is already done
          }
          if (track.sample_start_delay_ticks > tick_num && track.input_sample_offset == 0) {
            // Delay was requested via effect EDx and we shouldn't start sound yet
            continue;
          }
          if (track.sample_retrigger_interval_ticks &&
              ((tick_num % track.sample_retrigger_interval_ticks) == 0)) {
            track.input_sample_offset = 0;
          }

          uint16_t effective_period = track.period;
          if (track.vibrato_amplitude && track.vibrato_cycles) {
            float integer_part;
            float wave_progress = modff(track.vibrato_offset, &integer_part);
            float tick_vibrato_amplitude;
            switch (track.vibrato_waveform & 3) {
              case 0: // Sine wave
              case 3: // Supposedly random, but that would probably sound weird
                tick_vibrato_amplitude = sinf(wave_progress * 2 * M_PI);
                break;
              case 1: // Descending sawtooth wave
                tick_vibrato_amplitude = 1.0 - (2.0 * wave_progress);
                break;
              case 2: // Square wave
                tick_vibrato_amplitude = (wave_progress < 0.5) ? 1.0 : -1.0;
                break;
            }
            tick_vibrato_amplitude *= static_cast<float>(track.vibrato_amplitude) / 16.0;
            effective_period *= pow(2, -tick_vibrato_amplitude / 12.0);
          }

          // Handle arpeggio effects, which can change a sample's period within
          // a tick. To handle this, we further divide each tick into "segments"
          // where different periods can be used.
          size_t division_output_offset = tick_num * tick_samples.size();
          // This is a list of (start_at_output_sample, instrument_period) for
          // the current tick
          vector<pair<size_t, uint16_t>> segments({{0, effective_period}});
          if (track.arpeggio_arg) {
            // We multiply by 2 and 4 here since the output is stereo
            segments.emplace_back(make_pair(
                2 * timing.samples_per_tick * timing.ticks_per_division / 3,
                track.period / pow(2, ((track.arpeggio_arg >> 4) & 0x0F) / 12.0)));
            segments.emplace_back(make_pair(
                4 * timing.samples_per_tick * timing.ticks_per_division / 3,
                track.period / pow(2, (track.arpeggio_arg & 0x0F) / 12.0)));
          }

          // Apply the appropriate portion of the instrument's sample data to the
          // tick output data
          float track_volume_factor = static_cast<float>(track.volume) / 64.0;
          float ins_volume_factor = static_cast<float>(i.volume) / 64.0;
          const vector<float>* resampled_data = nullptr;
          ssize_t segment_index = -1;
          double src_ratio = -1.0;
          double resampled_offset = -1.0;
          double loop_start_offset = -1.0;
          double loop_end_offset = -1.0;
          for (size_t tick_output_offset = 0;
               tick_output_offset < tick_samples.size();
               tick_output_offset += 2, division_output_offset += 2) {

            // Advance to the appropriate segment if there is one
            bool changed_segment = false;
            while ((segment_index < static_cast<ssize_t>(segments.size() - 1)) &&
                division_output_offset >= segments.at(segment_index + 1).first) {
              segment_index++;
            changed_segment = true;
            }
            if (changed_segment) {
              const auto& segment = segments.at(segment_index);

              // Resample the instrument to the appropriate pitch
              // TODO: For PAL Amiga, use 7093789.2 here instead. Should this be
              // configurable?
              // The input samples to be played per second is:
              // track_input_samples_per_second = 7159090.5 / (2 * period)
              // To convert this to the number of output samples per input sample,
              // all  we have to do is divide the output sample rate by it:
              // out_samples_per_in_sample = sample_rate / (7159090.5 / (2 * period))
              // out_samples_per_in_sample = (sample_rate * 2 * period) / 7159090.5
              // This gives how many samples to generate for each input sample.
              src_ratio =
                  static_cast<double>(2 * timing.sample_rate * segment.second) / 7159090.5;
              resampled_data = &sample_cache.resample_add(
                  track.instrument_num, i.sample_data, 1, src_ratio);
              resampled_offset = track.input_sample_offset * src_ratio;

              // The sample has a loop if the length in words is > 1. We convert words
              // to samples long before this point, so we have to check for >2 here.
              loop_start_offset = static_cast<double>(i.loop_start_samples) * src_ratio;
              loop_end_offset = (i.loop_length_samples > 2)
                  ? static_cast<double>(i.loop_start_samples + i.loop_length_samples) * src_ratio
                  : 0.0;
            }

            if (!resampled_data) {
              throw logic_error("resampled data not present at sound generation time");
            }

            // The sample could "end" here (and not below) because of
            // floating-point imprecision
            if (resampled_offset >= resampled_data->size()) {
              if (loop_end_offset != 0.0) {
                // This should only happen if the loop ends right at the end of
                // the sample, so we can just blindly reset to the loop start
                // offset.
                throw runtime_error("sample ended after resample but loop is present");
                track.input_sample_offset = loop_start_offset / src_ratio;
              }
              track.input_sample_offset = i.sample_data.size();
              break;
            }
            float effective_sample = resampled_data->at(static_cast<size_t>(resampled_offset)) * track_volume_factor * ins_volume_factor;

            // Tracks 1 and 2 (mod 4) are on the right; the others are on the left
            // We only pan partway from center; 100% panning sounds bad
            if (((track_index & 3) == 1) || ((track_index & 3) == 2)) {
              tick_samples[tick_output_offset + 0] += effective_sample * 1.0 / 4.0; // L
              tick_samples[tick_output_offset + 1] += effective_sample * 3.0 / 4.0; // R
            } else {
              tick_samples[tick_output_offset + 0] += effective_sample * 3.0 / 4.0; // L
              tick_samples[tick_output_offset + 1] += effective_sample * 1.0 / 4.0; // R
            }

            // TODO: The observational spec claims that the loop only begins after
            // the sample has been played to the end once. Is this true? It seems
            // like we should instead always jump back when we reach the end of
            // the loop region, even the first time we reach it (which is what's
            // implemented here).
            resampled_offset++;
            if (loop_end_offset != 0.0 && resampled_offset >= loop_end_offset) {
              resampled_offset = loop_start_offset;
            }

            // Advance the input offset by a proportional amount to the sound we
            // just generated, so the next tick will start at the right place
            track.input_sample_offset = resampled_offset / src_ratio;

            if (resampled_offset >= resampled_data->size()) {
              break;
            }
          }
          if (track.per_tick_period_increment) {
            track.period += track.per_tick_period_increment;
            // If a slide to note effect (3) is underway, enforce the limit given
            // by the effect command
            if (track.slide_target_period &&
                (((track.per_tick_period_increment > 0) &&
                  (track.period > track.slide_target_period)) ||
                 ((track.per_tick_period_increment < 0) &&
                  (track.period < track.slide_target_period)))) {
              track.period = track.slide_target_period;
              track.per_tick_period_increment = 0;
              track.slide_target_period = 0;
            }
            // TODO: implement limits here (they may be different across trackers)
            if (track.period <= 0) {
              track.period = 1;
            }
          }
          if (track.per_tick_volume_increment) {
            track.volume += track.per_tick_volume_increment;
            if (track.volume < 0) {
              track.volume = 0;
            } else if (track.volume > 64) {
              track.volume = 64;
            }
          }
          // TODO: Should we advance this only sometimes? What would the right
          // condition be here?
          track.vibrato_offset += static_cast<float>(track.vibrato_cycles) / 64;
        }
        total_output_samples += tick_samples.size();
        on_tick_complete(move(tick_samples));
      }

      // Clear division-scoped effects on all tracks
      for (auto& track : tracks) {
        track.reset_division_scoped_effects();
      }
    }

    // Advance to the next cell or pattern, or skip to the next pattern if a
    // pattern break was given
    if (pattern_break_target >= 0) {
      current_cell = pattern_break_target;
      current_partition++;
      pattern_loop_start = 0;
      if (current_partition >= mod.partition_count) {
        return;
      }
      if (current_cell >= 64) {
        throw runtime_error("pattern break opcode jumps past end of next pattern");
      }
    } else if (jump_to_pattern_loop_start) {
      current_cell = pattern_loop_start;
      jump_to_pattern_loop_start = false;
    } else {
      current_cell++;
      if (current_cell >= 64) {
        current_cell = 0;
        current_partition++;
        pattern_loop_start = 0;
      }
    }
  }
}

vector<float> render_mod(
    const Module& mod,
    size_t output_sample_rate,
    int resample_method,
    size_t skip_partitions,
    uint64_t flags) {
  size_t all_samples_count = 0;
  deque<vector<float>> all_tick_data;
  render_mod(mod, output_sample_rate, resample_method, skip_partitions, flags, [&](vector<float>&& tick_data) -> void {
    all_tick_data.emplace_back(move(tick_data));
  });

  vector<float> ret;
  ret.reserve(all_samples_count);
  for (const auto& tick_data : all_tick_data) {
    ret.insert(ret.end(), tick_data.begin(), tick_data.end());
  }
  return ret;
}



void normalize_amplitude(vector<float>& data) {
  float max_amplitude = 0.0f;
  for (float sample : data) {
    if (sample > max_amplitude) {
      max_amplitude = sample;
    }
    if (sample < -max_amplitude) {
      max_amplitude = -sample;
    }
  }

  fprintf(stderr, "normalizing by %lg\n", max_amplitude);
  if (max_amplitude == 0.0) {
    return;
  }
  for (float& sample : data) {
    sample /= max_amplitude;
  }
}



void print_usage() {
  fprintf(stderr, "\
\n\
modsynth - a synthesizer for ProTracker modules\n\
\n\
Usage:\n\
  modsynth --disassemble [options] input_filename\n\
    Disassembles the instruments and sequence program from the module.\n\
    Options:\n\
      --show-sample-data: Shows raw sample data in a hex/ASCII view.\n\
      --show-sample-saveforms: Shows sample waveforms vertically. If color is\n\
          enabled, possibly-clipped samples are highlighted in red.\n\
\n\
  modsynth --export-instruments input_filename\n\
    Exports the instruments from the module. Each instrument has at most one\n\
    sample. Each sample is saved as <input_filename>_<instrument_number>.wav.\n\
    Samples are converted to 32-bit floating-point format during export.\n\
\n\
  modsynth --render [options] input_filename\n\
    Generates a rasterized version of the sequence. Saves the result as\n\
    <input_filename>.wav. Options:\n\
      --skip-partitions=N: Start at this offset in the partition table (instead\n\
          of zero).\n\
      --sample-rate=N: Output audio at this sample rate (default 48000).\n\
\n\
  modsynth --play [options] input_filename\n\
    Plays the sequence through the default audio device.\n\
    All options to --render apply here too. Additional options:\n\
      --play-buffers=N: Generate this many ticks of audio ahead of the output\n\
          device (default 32). If audio is choppy, try increasing this value.\n\
\n");
}

int main(int argc, char** argv) {
  enum class Behavior {
    Disassemble,
    ExportInstruments,
    Render,
    Play,
  };

  Behavior behavior = Behavior::Disassemble;
  const char* input_filename = nullptr;
  size_t output_sample_rate = 48000;
  size_t num_play_buffers = 64;
  size_t skip_partitions = 0;
  bool use_default_color_flags = true;
  uint64_t flags = Flags::Default;
  for (int x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "--disassemble")) {
      behavior = Behavior::Disassemble;
    } else if (!strcmp(argv[x], "--export-instruments")) {
      behavior = Behavior::ExportInstruments;
    } else if (!strcmp(argv[x], "--render")) {
      behavior = Behavior::Render;
    } else if (!strcmp(argv[x], "--play")) {
      behavior = Behavior::Play;

    } else if (!strcmp(argv[x], "--no-color")) {
      flags &= ~Flags::TerminalColor;
      use_default_color_flags = false;
    } else if (!strcmp(argv[x], "--color")) {
      flags |= Flags::TerminalColor;
      use_default_color_flags = false;
    } else if (!strcmp(argv[x], "--show-sample-data")) {
      flags |= Flags::ShowSampleData;
    } else if (!strcmp(argv[x], "--show-sample-waveforms")) {
      flags |= Flags::ShowSampleWaveforms;

    } else if (!strncmp(argv[x], "--skip-partitions=", 18)) {
      skip_partitions = atoi(&argv[x][18]);
    } else if (!strncmp(argv[x], "--play-buffers=", 15)) {
      num_play_buffers = atoi(&argv[x][15]);
    } else if (!strncmp(argv[x], "--sample-rate=", 14)) {
      output_sample_rate = atoi(&argv[x][14]);

    } else if (!input_filename) {
      input_filename = argv[x];

    } else {
      fprintf(stderr, "error: multiple filenames given, or unknown option: %s\n",
          argv[x]);
      print_usage();
      return 1;
    }
  }
  if (!input_filename) {
    fprintf(stderr, "error: no input filename given\n");
    print_usage();
    return 1;
  }

  if (use_default_color_flags &&
      isatty(fileno((behavior == Behavior::Disassemble) ? stdout : stderr))) {
    flags |= Flags::TerminalColor;
  }

  unique_ptr<Module> mod;
  {
    string input_data = load_file(input_filename);
    StringReader r(input_data.data(), input_data.size());
    mod.reset(new Module(load_mod(r)));
  }

  switch (behavior) {
    case Behavior::Disassemble:
      disassemble_mod(stdout, *mod, flags);
      break;
    case Behavior::ExportInstruments:
      export_mod_instruments(*mod, output_sample_rate, input_filename);
      break;
    case Behavior::Render: {
      string output_filename = string(input_filename) + ".wav";
      auto result = render_mod(*mod, output_sample_rate, SRC_SINC_BEST_QUALITY, skip_partitions, flags);
      // normalize_amplitude(result);
      save_wav(output_filename.c_str(), result, output_sample_rate, 2);
      break;
    }
    case Behavior::Play: {
      init_al();
      AudioStream stream(output_sample_rate, format_for_name("stereo-f32"), num_play_buffers);
      render_mod(*mod, output_sample_rate, SRC_LINEAR, skip_partitions, flags, [&](vector<float>&& tick_samples) {
        stream.check_buffers();
        stream.add_samples(tick_samples.data(), tick_samples.size());
      });
      fprintf(stderr, "waiting for buffers to drain\n");
      stream.wait();
      exit_al();
      break;
    }
    default:
      throw logic_error("invalid behavior");
  }

  return 0;
}