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
  ShowUnusedPatterns  = 0x08,
  ShowLoadingDebug    = 0x10,
  Default             = 0x00,
};

struct Module {
  struct Instrument {
    size_t index;
    string name;
    uint32_t num_samples;
    int8_t finetune; // Pitch shift (positive or negative) in increments of 1/8 semitone
    uint8_t volume; // 0-64
    uint16_t loop_start_samples;
    uint16_t loop_length_samples;
    vector<int8_t> original_sample_data;
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



int8_t sign_extend_nybble(int8_t x) {
  if (x & 0x08) {
    return x | 0xF0;
  } else {
    return x & 0x0F;
  }
}

// Forward declaration for debugging purposes
void disassemble_mod(FILE* stream, shared_ptr<const Module> mod, uint64_t flags);

shared_ptr<Module> parse_mod(const string& data, uint64_t flags) {
  StringReader r(data.data(), data.size());

  // Check for other known file type signatures, but don't let them prevent
  // attempting to load the file
  if (r.read(3, false) == "MAD") {
    fprintf(stderr, "Warning: this file may be a MAD file, not a MOD\n");
  } else if (r.read(17, false) == "Extended Module: ") {
    fprintf(stderr, "Warning: this file may be an XM file, not a MOD\n");
  } else if (r.read(4, false) == "IMPM") {
    fprintf(stderr, "Warning: this file may be an IT file, not a MOD\n");
  } else if (r.read(3, false) == "MTM") {
    fprintf(stderr, "Warning: this file may be an MTM file, not a MOD\n");
  } else if (r.pread(0x2C, 4) == "SCRM") {
    fprintf(stderr, "Warning: this file may be an S3M file, not a MOD\n");
  }

  shared_ptr<Module> mod(new Module());

  // First, look ahead to see if this file uses any extensions. Annoyingly, the
  // signature field is pretty late in the file format, and some preceding
  // fields' sizes depend on the enabled extensions.
  try {
    mod->extension_signature = r.pget_u32r(0x438);
  } catch (const out_of_range&) {
    mod->extension_signature = 0;
  }

  size_t num_instruments = 31; // This is only not 31 in the default case below
  switch (mod->extension_signature) {
    case 0x4D2E4B2E: // M.K.
    case 0x4D214B21: // M!K!
    case 0x464C5434: // FLT4
    case 0x464C5438: // FLT8
      // Note: the observational spec appears to be incorrect about the FLT8
      // case - MODs with that signature appear to have only 4 channels.
      mod->num_tracks = 4;
      break;
    default:
      if ((mod->extension_signature & 0xF0FFFFFF) == 0x3043484E) { // xCHN
        mod->num_tracks = (mod->extension_signature >> 24) & 0x0F;
      } else if ((mod->extension_signature & 0xF0F0FFFF) == 0x30304348) { // xxCH
        mod->num_tracks = (((mod->extension_signature >> 24) & 0x0F) * 10) +
            ((mod->extension_signature >> 16) & 0x0F);
      } else { // Unrecognized signature; probably a very old MOD
        num_instruments = 15;
        mod->num_tracks = 4;
      }
  }

  if (flags & Flags::ShowLoadingDebug) {
    fprintf(stderr, "Loader[%zX]: extension signature is %08X (%zu tracks, %zu instruments)\n",
        r.where(), mod->extension_signature, mod->num_tracks, num_instruments);
  }

  mod->name = r.read(0x14);
  strip_trailing_zeroes(mod->name);

  if (flags & Flags::ShowLoadingDebug) {
    string escaped_name = escape_quotes(mod->name);
    fprintf(stderr, "Loader[%zX]: name is \"%s\"\n",
        r.where(), escaped_name.c_str());
  }

  mod->instruments.resize(num_instruments);
  for (size_t x = 0; x < num_instruments; x++) {
    auto& i = mod->instruments[x];
    i.index = x;
    i.name = r.read(0x16);
    strip_trailing_zeroes(i.name);
    i.num_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
    i.finetune = sign_extend_nybble(r.get_u8());
    i.volume = r.get_u8();
    i.loop_start_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
    i.loop_length_samples = static_cast<uint32_t>(r.get_u16r()) << 1;
    if (flags & Flags::ShowLoadingDebug) {
      fprintf(stderr, "Loader[%zX]: loaded instrument %zu (0x%X samples to read)\n",
          r.where(), x + 1, i.num_samples);
    }
  }

  mod->partition_count = r.get_u8();
  r.get_u8(); // unused
  r.read_into(mod->partition_table.data(), mod->partition_table.size());
  if (flags & Flags::ShowLoadingDebug) {
    fprintf(stderr, "Loader[%zX]: loaded partition table (%hhu/128 partitions)\n",
        r.where(), mod->partition_count);
  }

  // We should have gotten to exactly the same offset that we read ahead to at
  // the beginning, unless there were not 31 instruments.
  if (num_instruments == 31) {
    uint32_t inplace_extension_signature = r.get_u32r();
    if (mod->extension_signature &&
        mod->extension_signature != inplace_extension_signature) {
      if (flags & Flags::ShowLoadingDebug) {
        fprintf(stderr, "Loader[%zX]: Loaded so far:\n", r.where());
        disassemble_mod(stderr, mod, flags);
      }
      throw logic_error("read-ahead extension signature does not match inplace extension signature");
    }
    if (flags & Flags::ShowLoadingDebug) {
      fprintf(stderr, "Loader[%zX]: inplace extension signature ok\n", r.where());
    }
  }

  // Compute the number of patterns based on the contents of the partition
  // table. The number of patterns is the maximum value in the table (+1, since
  // pattern 0 is valid), and even patterns that do not appear in this table but
  // are less than the maximum value will exist in the file. Some rare MODs have
  // unreferenced patterns in the unused space after the used partitions; we
  // have to iterate the entire table (not just up to mod->partition_count) to
  // account for those as well.
  size_t num_patterns = 0;
  for (size_t x = 0; x < 0x80; x++) {
    if (num_patterns <= mod->partition_table[x]) {
      num_patterns = mod->partition_table[x] + 1;
    }
  }
  if (flags & Flags::ShowLoadingDebug) {
    fprintf(stderr, "Loader[%zX]: there are %zu patterns\n", r.where(), num_patterns);
  }

  // Load the patterns.
  mod->patterns.resize(num_patterns);
  for (size_t x = 0; x < num_patterns; x++) {
    auto& pat = mod->patterns[x];
    pat.divisions.resize(mod->num_tracks * 64);
    for (auto& div : pat.divisions) {
      div.wx = r.get_u16r();
      div.yz = r.get_u16r();
    }
    if (flags & Flags::ShowLoadingDebug) {
      fprintf(stderr, "Loader[%zX]: loaded pattern %zu\n", r.where(), x);
    }
  }

  // Load the sample data for each instrument.
  for (auto& i : mod->instruments) {
    i.original_sample_data.resize(i.num_samples);
    if (r.read_into(i.original_sample_data.data(), i.num_samples) != i.num_samples) {
      fprintf(stderr, "Warning: sound data is missing for instrument %zu\n",
          i.index + 1);
    }
    i.sample_data.resize(i.num_samples);
    for (size_t x = 0; x < i.num_samples; x++) {
      int8_t sample = i.original_sample_data[x];
      i.sample_data[x] = (sample == -0x80)
          ? -1.0f
          : (static_cast<float>(sample) / 128.0f);
    }
    if (flags & Flags::ShowLoadingDebug) {
      fprintf(stderr, "Loader[%zX]: loaded samples for instrument %zu\n",
          r.where(), i.index + 1);
    }
  }

  return mod;
}

shared_ptr<Module> load_mod(const string& filename, uint64_t flags) {
  return parse_mod(load_file(filename), flags);
}



void disassemble_pattern_row(
    FILE* stream,
    shared_ptr<const Module> mod,
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

  static const TerminalFormat track_colors[5] = {
    TerminalFormat::FG_RED,
    TerminalFormat::FG_CYAN,
    TerminalFormat::FG_YELLOW,
    TerminalFormat::FG_GREEN,
    TerminalFormat::FG_MAGENTA,
  };
  bool use_color = flags & Flags::TerminalColor;

  const auto& p = mod->patterns.at(pattern_num);
  fprintf(stream, "  %02hhu +%2hhu", pattern_num, y);
  for (size_t z = 0; z < mod->num_tracks; z++) {
    const auto& div = p.divisions[y * mod->num_tracks + z];
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
          print_color_escape(stream, track_colors[z % 5], TerminalFormat::BOLD, TerminalFormat::END);
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

void print_mod_text(FILE* stream, shared_ptr<const Module> mod) {
  fprintf(stream, "Name: %s\n", mod->name.c_str());
  fprintf(stream, "Instruments/Notes:\n");
  for (const auto& i : mod->instruments) {
    if (i.name.empty() && i.sample_data.empty()) {
      continue;
    }
    string escaped_name = escape_quotes(i.name);
    fprintf(stream, "  [%02zu] %s\n", i.index + 1, escaped_name.c_str());
  }
}

void disassemble_mod(FILE* stream, shared_ptr<const Module> mod, uint64_t flags) {
  fprintf(stream, "Name: %s\n", mod->name.c_str());
  fprintf(stream, "Tracks: %zu\n", mod->num_tracks);
  fprintf(stream, "Instruments: %zu\n", mod->instruments.size());
  fprintf(stream, "Partitions: %hhu\n", mod->partition_count);
  fprintf(stream, "Extension signature: %08X\n", mod->extension_signature);

  for (const auto& i : mod->instruments) {
    fputc('\n', stream);
    string escaped_name = escape_quotes(i.name);
    fprintf(stream, "Instrument %zu: %s\n", i.index + 1, escaped_name.c_str());
    fprintf(stream, "  Fine-tune: %c%d/8 semitones\n",
        (i.finetune < 0) ? '-' : '+', (i.finetune < 0) ? -i.finetune : i.finetune);
    fprintf(stream, "  Volume: %hhu/64\n", i.volume);
    fprintf(stream, "  Loop: start at %hu for %hu samples\n", i.loop_start_samples, i.loop_length_samples);
    fprintf(stream, "  Data: (%zu samples)\n", i.sample_data.size());

    if (flags & Flags::ShowSampleData) {
      print_data(stream, i.original_sample_data.data(), i.original_sample_data.size());
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
        fprintf(stream, "  ins %02zu +%04zX [%s]%s\n", i.index + 1, z, line_data.c_str(), suffix);
        if (((sample == -0x80) || (sample == 0x7F)) && (flags & Flags::TerminalColor)) {
          print_color_escape(stream, TerminalFormat::NORMAL, TerminalFormat::END);
        }
        line_data[offset] = ' ';
      }
    }
  }

  vector<bool> patterns_used(0x80, !!(flags & Flags::ShowUnusedPatterns));
  for (size_t x = 0; x < mod->partition_count; x++) {
    patterns_used.at(mod->partition_table.at(x)) = true;
  }

  for (size_t x = 0; x < mod->patterns.size(); x++) {
    if (!patterns_used.at(x)) {
      continue;
    }
    fputc('\n', stream);
    fprintf(stream, "Pattern %zu\n", x);
    for (size_t y = 0; y < 64; y++) {
      disassemble_pattern_row(stream, mod, x, y, flags);
      fputc('\n', stream);
    }
  }

  fputc('\n', stream);
  fprintf(stream, "Partition table:\n");
  for (size_t x = 0; x < mod->partition_count; x++) {
    fprintf(stream, "  Partition %zu: %hu\n", x, mod->partition_table[x]);
  }
}



void export_mod_instruments(
    shared_ptr<const Module> mod, size_t output_sample_rate, const char* output_prefix) {
  // Andrew's observational spec notes that about 8287 bytes of data are sent to
  // the channel per second when a normal sample is played at C2. Empirically,
  // it seems like this is 0.5x the sample rate we need to make music sound
  // normal. Maybe the spec should have said 8287 words were sent to the channel
  // per second instead?
  for (const auto& i : mod->instruments) {
    if (i.sample_data.empty()) {
      fprintf(stderr, "... (%zu) \"%s\" -> (no sound data)\n",
          i.index + 1,
          i.name.c_str());
    } else {
      string escaped_name = escape_quotes(i.name);
      fprintf(stderr, "... (%zu) \"%s\" -> %zu samples, +%hhdft, %02hhX vol, loop [%hux%hu]\n",
          i.index + 1,
          escaped_name.c_str(),
          i.sample_data.size(),
          i.finetune,
          i.volume,
          i.loop_start_samples,
          i.loop_length_samples);

      string output_filename_u8 = string_printf("%s_%zu.u8.wav", output_prefix, i.index + 1);
      vector<uint8_t> u8_sample_data;
      u8_sample_data.reserve(i.num_samples);
      for (int8_t sample : i.original_sample_data) {
        u8_sample_data.emplace_back(static_cast<uint8_t>(sample) - 0x80);
      }
      save_wav(output_filename_u8.c_str(), u8_sample_data, 16574, 1);

      string output_filename_f32 = string_printf("%s_%zu.f32.wav", output_prefix, i.index + 1);
      save_wav(output_filename_f32.c_str(), i.sample_data, 16574, 1);
    }
  }
}



class MODSynthesizer {
public:
  struct Options {
    double amiga_hardware_frequency;
    size_t output_sample_rate;
    int resample_method;
    float global_volume;
    float max_output_seconds;
    size_t skip_partitions;
    bool allow_backward_position_jump;
    unordered_set<size_t> mute_tracks;
    unordered_set<size_t> solo_tracks;
    float tempo_bias;
    size_t arpeggio_frequency; // 0 = use ticks instead
    size_t vibrato_resolution;
    uint64_t flags;

    Options()
      : amiga_hardware_frequency(7159090.5),
        output_sample_rate(48000),
        resample_method(SRC_SINC_BEST_QUALITY),
        global_volume(1.0),
        max_output_seconds(0.0),
        skip_partitions(0),
        allow_backward_position_jump(false),
        mute_tracks(),
        solo_tracks(),
        tempo_bias(1.0),
        arpeggio_frequency(0),
        vibrato_resolution(1),
        flags(Flags::Default) { }
  };


protected:
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
    size_t index;

    int32_t instrument_num; // 1-based! 0 = no instrument
    int32_t period;
    int32_t volume; // 0 - 64
    int32_t panning; // 0 (left) - 128 (right)
    bool enable_surround_effect;
    int8_t finetune_override; // -0x80 = use instrument finetune (default)
    double input_sample_offset; // relative to input samples, not any resampling thereof
    uint8_t vibrato_waveform;
    uint8_t tremolo_waveform;
    float vibrato_offset;
    float tremolo_offset;

    uint8_t arpeggio_arg;
    uint8_t sample_retrigger_interval_ticks;
    uint8_t sample_start_delay_ticks;
    int8_t cut_sample_after_ticks;
    int32_t delayed_sample_instrument_num;
    int32_t delayed_sample_period;
    int16_t per_tick_period_increment;
    int16_t per_tick_volume_increment;
    int16_t slide_target_period;
    int16_t vibrato_amplitude;
    int16_t tremolo_amplitude;
    int16_t vibrato_cycles;
    int16_t tremolo_cycles;
    // These are not reset each division, and are used for effects that continue a
    // previous effect
    int16_t last_slide_target_period;
    int16_t last_per_tick_period_increment;
    int16_t last_vibrato_amplitude;
    int16_t last_tremolo_amplitude;
    int16_t last_vibrato_cycles;
    int16_t last_tremolo_cycles;

    float last_sample;
    float dc_offset;
    bool next_sample_may_be_discontinuous;

    TrackState()
      : index(0),
        instrument_num(0),
        period(0),
        volume(64),
        panning(64),
        enable_surround_effect(false),
        finetune_override(-0x80),
        input_sample_offset(0.0),
        vibrato_waveform(0),
        tremolo_waveform(0),
        vibrato_offset(0.0),
        tremolo_offset(0.0),
        last_slide_target_period(0),
        last_per_tick_period_increment(0),
        last_vibrato_amplitude(0),
        last_tremolo_amplitude(0),
        last_vibrato_cycles(0),
        last_tremolo_cycles(0),
        last_sample(0),
        dc_offset(0),
        next_sample_may_be_discontinuous(false) {
      this->reset_division_scoped_effects();
    }

    void reset_division_scoped_effects() {
      this->arpeggio_arg = 0;
      this->sample_retrigger_interval_ticks = 0;
      this->sample_start_delay_ticks = 0;
      this->cut_sample_after_ticks = -1;
      this->delayed_sample_instrument_num = 0;
      this->delayed_sample_period = 0;
      this->per_tick_period_increment = 0;
      this->per_tick_volume_increment = 0;
      this->slide_target_period = 0;
      this->vibrato_amplitude = 0;
      this->tremolo_amplitude = 0;
      this->vibrato_cycles = 0;
      this->tremolo_cycles = 0;
    }

    void start_note(int32_t instrument_num, int32_t period, int32_t volume) {
      this->instrument_num = instrument_num;
      this->period = period;
      this->volume = volume;
      this->finetune_override = -0x80;
      this->input_sample_offset = 0.0;
      if (!(this->vibrato_waveform & 4)) {
        this->vibrato_offset = 0.0;
      }
      if (!(this->tremolo_waveform & 4)) {
        this->tremolo_offset = 0.0;
      }
      this->set_discontinuous_flag();
    }

    void set_discontinuous_flag() {
      this->dc_offset = this->last_sample;
      this->next_sample_may_be_discontinuous = true;
    }

    void decay_dc_offset(float delta) {
      if (this->dc_offset > 0) {
        if (this->dc_offset <= delta) {
          this->dc_offset = 0;
        } else {
          this->dc_offset -= delta;
        }
      } else if (this->dc_offset < 0) {
        if (this->dc_offset >= -delta) {
          this->dc_offset = 0;
        } else {
          this->dc_offset += delta;
        }
      }
    }
  };

  shared_ptr<const Module> mod;
  shared_ptr<const Options> opts;
  size_t max_output_samples;
  Timing timing;
  vector<TrackState> tracks;
  SampleCache<uint8_t> sample_cache;

  size_t partition_index;
  ssize_t division_index;
  ssize_t pattern_loop_start_index;
  ssize_t pattern_loop_times_remaining;
  bool jump_to_pattern_loop_start;
  size_t total_output_samples;
  int32_t pattern_break_target;
  int32_t partition_break_target;
  vector<bool> partitions_executed;
  ssize_t divisions_to_delay;

  float dc_offset_decay;

  virtual void on_tick_samples_ready(vector<float>&&) = 0;

public:
  MODSynthesizer(shared_ptr<const Module> mod, shared_ptr<const Options> opts)
    : mod(mod),
      opts(opts),
      max_output_samples(0),
      timing(this->opts->output_sample_rate),
      tracks(this->mod->num_tracks),
      sample_cache(this->opts->resample_method),
      partition_index(this->opts->skip_partitions),
      division_index(0),
      pattern_loop_start_index(0),
      pattern_loop_times_remaining(-1),
      jump_to_pattern_loop_start(false),
      total_output_samples(0),
      pattern_break_target(-1),
      partition_break_target(-1),
      partitions_executed(0x80, false),
      divisions_to_delay(0),
      dc_offset_decay(0.001) {
    // Initialize track state which depends on track index
    for (size_t x = 0; x < this->tracks.size(); x++) {
      this->tracks[x].index = x;
      // Tracks 1 and 2 (mod 4) are on the right; the others are on the left.
      // These assignments can be overridden by a [14][8][x] (0xE8x) effect.
      this->tracks[x].panning = ((x & 3) == 1) || ((x & 3) == 2) ? 0x60 : 0x20;
    }
  }

protected:
  void show_current_division(bool changed_partition) const {
    uint8_t pattern_index = this->mod->partition_table.at(this->partition_index);
    fputs("  ", stderr);
    if (changed_partition && (this->opts->flags & Flags::TerminalColor)) {
      print_color_escape(stderr, TerminalFormat::FG_WHITE, TerminalFormat::BOLD, TerminalFormat::INVERSE, TerminalFormat::END);
    }
    if (this->partition_index < 10) {
      fprintf(stderr, " %1zu ", this->partition_index);
    } else {
      fprintf(stderr, "%3zu", this->partition_index);
    }
    if (changed_partition && (this->opts->flags & Flags::TerminalColor)) {
      print_color_escape(stderr, TerminalFormat::NORMAL, TerminalFormat::END);
    }
    disassemble_pattern_row(
        stderr,
        this->mod,
        pattern_index,
        this->division_index,
        this->opts->flags);
    float time = static_cast<float>(this->total_output_samples) /
          (2 * this->opts->output_sample_rate);
    fprintf(stderr, "  =  %3zu/%-2zu @ %.7gs\n",
        this->timing.beats_per_minute, this->timing.ticks_per_division, time);
  }

  const Module::Pattern& current_pattern() const {
    uint8_t pattern_index = this->mod->partition_table.at(this->partition_index);
    return this->mod->patterns.at(pattern_index);
  }

  void execute_current_division_commands() {
    this->pattern_break_target = -1;
    this->partition_break_target = -1;
    this->divisions_to_delay = 0;
    const auto& pattern = this->current_pattern();
    for (auto& track : this->tracks) {
      const auto& div = pattern.divisions.at(
          this->division_index * this->mod->num_tracks + track.index);

      uint16_t effect = div.effect();
      uint16_t div_period = div.period();
      uint8_t div_ins_num = div.instrument_num();

      if ((effect & 0xFF0) != 0xED0) {
        // If an instrument number is given, update the track's instrument and
        // reset the track's volume. It appears this should happen even if the
        // note is not played due to an effect 3xx or 5xx, but it probably should
        // NOT happen if there's an effect EDx.
        if (div_ins_num) {
          track.volume = 64;
        }

        // There are surprisingly many cases for when a note should start vs. not
        // start, and different behavior for each. It seems correct behavior is:
        // 1. Period given, ins_num given: start a new note
        // 2. Period given, ins_num missing: start a new note with old ins_num
        //    and old volume
        // 3. Period missing, ins_num given and matches old ins_num: reset volume
        //    only (this is already done above)
        // 4. Period missing, ins_num given and does not match old ins_num: start
        //    a new note, unless old ins_num is zero, in which case just set the
        //    track's ins_num for future notes
        // 5. Period and ins_num both missing: do nothing
        // Effects [3] and [5] are special cases and do not result in a new note
        // being played, since they use the period as an additional parameter.
        // Effect [14][13] is special in that it does not start the new note
        // immediately, and the existing note, if any, should continue playing for
        // at least another tick.
        if (((effect & 0xF00) != 0x300) && ((effect & 0xF00) != 0x500) &&
            (div_period || // Cases (1) and (2)
             (div_ins_num && (div_ins_num != track.instrument_num)))) { // Case (4)
          uint16_t note_period = div_period ? div_period : track.period;
          uint8_t note_ins_num = div_ins_num ? div_ins_num : track.instrument_num;
          // We already reset the track's volume above if ins_num is given. If
          // ins_num is not given, we should use the previous note volume anyway.
          track.start_note(note_ins_num, note_period, track.volume);
        }
      }

      switch (effect & 0xF00) {
        case 0x000: // Arpeggio (or no effect)
          track.arpeggio_arg = effect & 0x0FF;
          break;

        case 0x100: // Slide up
          track.slide_target_period = 113;
          track.per_tick_period_increment = -(effect & 0x0FF);
          break;
        case 0x200: // Slide down
          track.slide_target_period = 856;
          track.per_tick_period_increment = effect & 0x0FF;
          break;
        case 0x300: // Slide to note
          track.slide_target_period = div_period;
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
          // target period.
          track.slide_target_period = div_period;
          if (!track.slide_target_period) {
            track.slide_target_period = track.last_slide_target_period;
          }
          track.per_tick_period_increment = track.last_per_tick_period_increment;
          goto VolumeSlideEffect;

        case 0x600: // Volume slide during vibrato
          track.vibrato_amplitude = track.last_vibrato_amplitude;
          track.vibrato_cycles = track.last_vibrato_cycles;
          goto VolumeSlideEffect;

        case 0x700: // Tremolo
          track.tremolo_amplitude = effect & 0x00F;
          if (!track.tremolo_amplitude) {
            track.tremolo_amplitude = track.last_tremolo_amplitude;
          } else {
            track.last_tremolo_amplitude = track.tremolo_amplitude;
          }
          track.tremolo_cycles = (effect & 0x0F0) >> 4;
          if (!track.tremolo_cycles) {
            track.tremolo_cycles = track.last_tremolo_cycles;
          } else {
            track.last_tremolo_cycles = track.tremolo_cycles;
          }
          break;

        case 0x800: // Panning
          track.panning = effect & 0x0FF;
          track.enable_surround_effect = (track.panning == 0xA4);
          if (track.panning > 0x80) {
            track.panning = 0x80;
          }
          break;

        case 0x900: { // Set sample offset
          // The spec says the parameter is essentially <<8 but is measured in
          // words. This appears to be false - PlayerPRO shifts by 8 here (not
          // 9), and the MODs I've tried sound wrong when using 9.
          track.input_sample_offset = static_cast<int32_t>(effect & 0x0FF) << 8;
          // If the instrument has a loop and the offset ie beyond the end of
          // the loop, jump to the start of the loop instead.
          const auto& i = this->mod->instruments.at(track.instrument_num - 1);
          if ((i.loop_length_samples > 2) &&
              (track.input_sample_offset >= i.loop_start_samples + i.loop_length_samples)) {
            track.input_sample_offset = i.loop_start_samples;
          }
          break;
        }

        VolumeSlideEffect:
        case 0xA00: // Volume slide
          if (effect & 0x0F0) {
            track.per_tick_volume_increment = (effect & 0x0F0) >> 4;
          } else {
            track.per_tick_volume_increment = -(effect & 0x00F);
          }
          break;

        case 0xB00: { // Position jump
          // Don't allow a jump into a partition that has already executed, to
          // prevent infinite loops.
          uint8_t target_partition = effect & 0x07F;
          if (this->opts->allow_backward_position_jump ||
              !this->partitions_executed.at(target_partition)) {
            this->partition_break_target = target_partition;
            this->pattern_break_target = 0;
          }
          break;
        }

        case 0xC00: // Set volume
          track.volume = effect & 0x0FF;
          if (track.volume > 64) {
            track.volume = 64;
          }
          track.set_discontinuous_flag();
          break;

        case 0xD00: // Pattern break
          // This was probably just a typo in the original Protracker, but it's
          // now propagated everywhere... the high 4 bits are multiplied by 10,
          // not 16.
          this->partition_break_target = this->partition_index + 1;
          this->pattern_break_target = (((effect & 0x0F0) >> 4) * 10) + (effect & 0x00F);
          break;

        case 0xE00: { // Sub-effects
          switch (effect & 0x0F0) {
            case 0x000: // Enable/disable hardware filter
              // This is a hardware command on some Amigas; it looks like
              // PlayerPRO doesn't implement it, so neither will we.
              break;

            case 0x010: // Fine slide up
              track.period -= effect & 0x00F;
              break;
            case 0x020: // Fine slide down
              track.period += effect & 0x00F;
              break;

            // TODO: Implement this effect. See MODs:
            //   Futurity (part two)
            //   mod.vir
            //   Proofless
            // [14][3]: Set glissando on/off
            // Where [14][3][x] means "set glissando ON if x is 1, OFF if x is 0".
            // Used in conjunction with [3] ('Slide to note'). If glissando is on,
            // then 'Slide to note' will slide in semitones, otherwise will
            // perform the default smooth slide.

            case 0x040: // Set vibrato waveform
              // Note: there are only 8 waveforms defined (at least in the MOD
              // spec) so we don't bother with bit 3
              track.vibrato_waveform = effect & 0x007;
              break;

            case 0x050: // Set finetune override
              track.finetune_override = sign_extend_nybble(effect & 0x00F);
              break;

            case 0x060: { // Loop pattern
              uint8_t times = effect & 0x00F;
              if (times == 0) {
                this->pattern_loop_start_index = this->division_index;
              } else if (pattern_loop_times_remaining == -1) {
                this->pattern_loop_times_remaining = times - 1;
                this->jump_to_pattern_loop_start = true;
              } else if (pattern_loop_times_remaining > 0) {
                this->pattern_loop_times_remaining--;
                this->jump_to_pattern_loop_start = true;
              } else {
                this->pattern_loop_times_remaining = -1;
              }
              break;
            }

            case 0x070: // Set tremolo waveform
              track.tremolo_waveform = effect & 0x007;
              break;

            case 0x080: { // Set panning (PlayerPRO)
              uint16_t panning = effect & 0x00F;

              // To deal with the "halves" of the range not being equal sizes,
              // we stretch out the right half a bit so [14][8][15] hits the
              // right side exactly.
              if (panning <= 8) {
                panning *= 16;
              } else {
                panning *= 17;
              }
              track.panning = (panning * 0x80) / 0xFF;

              if (track.panning < 0) {
                track.panning = 0;
              } else if (track.panning > 0x80) {
                track.panning = 0x80;
              }
              break;
            }

            case 0x090: // Retrigger sample every x ticks
              track.sample_retrigger_interval_ticks = effect & 0x0F;
              break;
            case 0x0A0: // Fine volume slide up
              track.volume += effect & 0x00F;
              if (track.volume > 64) {
                track.volume = 64;
              }
              break;
            case 0x0B0: // Fine volume slide up
              track.volume -= effect & 0x00F;
              if (track.volume < 0) {
                track.volume = 0;
              }
              break;
            case 0x0C0: // Cut sample after ticks
              track.cut_sample_after_ticks = effect & 0x00F;
              break;
            case 0x0D0: // Delay sample
              track.sample_start_delay_ticks = effect & 0x00F;
              track.delayed_sample_instrument_num = div_ins_num;
              track.delayed_sample_period = div_period;
              break;
            case 0x0E0: // Delay pattern
              this->divisions_to_delay = effect & 0x00F;
              break;

            // TODO: Implement this effect. See MODs:
            //   deepest space
            //   Gummisnoppis
            // [14][15]: Invert loop
            // Where [14][15][x] means "if x is greater than 0, then play the
            // current sample's loop upside down at speed x". Each byte in the
            // sample's loop will have its sign changed (negated). It will only
            // work if the sample's loop (defined previously) is not too big. The
            // speed is based on an internal table.

            default:
              goto UnimplementedEffect;
          }
          break;
        }

        case 0xF00: { // Set speed
          uint8_t v = effect & 0xFF;
          if (v <= 32) {
            if (v == 0) {
              v = 1;
            }
            this->timing = Timing(
                this->timing.sample_rate, this->timing.beats_per_minute, v);
          } else {
            this->timing = Timing(
                this->timing.sample_rate, v, this->timing.ticks_per_division);
          }
          break;
        }
        UnimplementedEffect:
        default:
          fprintf(stderr, "warning: unimplemented effect %03hX\n", effect);
      }
    }
  }

  static float get_vibrato_tremolo_wave_amplitude(float offset, uint8_t waveform) {
    float integer_part;
    float wave_progress = modff(offset, &integer_part);
    switch (waveform & 3) {
      case 0: // Sine wave
      case 3: // Supposedly random, but that would probably sound weird
        return sinf(wave_progress * 2 * M_PI);
      case 1: // Descending sawtooth wave
        return 1.0 - (2.0 * wave_progress);
      case 2: // Square wave
        return (wave_progress < 0.5) ? 1.0 : -1.0;
      default:
        throw logic_error("invalid vibrato/tremolo waveform");
    }
  }

  void render_current_division_audio() {
    for (size_t tick_num = 0; tick_num < this->timing.ticks_per_division; tick_num++) {
      size_t num_tick_samples;
      if (opts->tempo_bias != 1.0) {
        num_tick_samples = this->timing.samples_per_tick / opts->tempo_bias;
      } else {
        num_tick_samples = this->timing.samples_per_tick;
      }
      // Note: we do this multiplication after the above computation because
      // num_tick_samples must not be an odd number, so we don't want to *2
      // during the floating-point computation.
      num_tick_samples *= 2;
      vector<float> tick_samples(num_tick_samples);
      for (auto& track : this->tracks) {

        // If track is muted or another track is solo'd, don't play its sound
        if (this->opts->mute_tracks.count(track.index) ||
            (!this->opts->solo_tracks.empty() &&
             !this->opts->solo_tracks.count(track.index))) {
          track.last_sample = 0;
          continue;
        }

        if (track.sample_start_delay_ticks &&
            (track.sample_start_delay_ticks == tick_num)) {
          // Delay requested via effect EDx and we should start the sample now
          track.start_note(track.delayed_sample_instrument_num, track.delayed_sample_period, 64);
          track.sample_start_delay_ticks = 0;
          track.delayed_sample_instrument_num = 0;
          track.delayed_sample_period = 0;
        }

        if (track.instrument_num == 0 || track.period == 0) {
          track.last_sample = 0;
          continue; // Track has not played any sound yet
        }

        const auto& i = this->mod->instruments.at(track.instrument_num - 1);
        if (track.input_sample_offset >= i.sample_data.size()) {
          track.last_sample = 0;
          continue; // Previous sound is already done
        }

        if (track.sample_retrigger_interval_ticks &&
            ((tick_num % track.sample_retrigger_interval_ticks) == 0)) {
          track.input_sample_offset = 0;
        }
        if ((track.cut_sample_after_ticks >= 0) &&
            (tick_num == track.cut_sample_after_ticks)) {
          track.volume = 0;
        }

        uint16_t effective_period = track.period;
        int8_t finetune = (track.finetune_override == -0x80) ? i.finetune : track.finetune_override;
        if (finetune) {
          effective_period *= pow(2, -static_cast<float>(finetune) / (12.0 * 8.0));
        }

        // Handle arpeggio and vibrato effects, which can change a sample's
        // period within a tick. To handle this, we further divide each division
        // into "segments" where different periods can be used. Segments can
        // cross tick boundaries, which makes the sample generation loop below
        // unfortunately rather complicated.
        size_t division_output_offset = tick_num * tick_samples.size();
        // This is a list of (start_at_output_sample, instrument_period) for
        // the current tick
        vector<pair<size_t, uint16_t>> segments;
        if (track.vibrato_amplitude && track.vibrato_cycles) {
          if (track.arpeggio_arg) {
            throw logic_error("cannot have both arpeggio and vibrato effects in the same division");
          }
          for (size_t x = 0; x < this->opts->vibrato_resolution; x++) {
            float amplitude = this->get_vibrato_tremolo_wave_amplitude(
                track.vibrato_offset + static_cast<float>(track.vibrato_cycles) / (64 * this->opts->vibrato_resolution), track.vibrato_waveform);
            amplitude *= static_cast<float>(track.vibrato_amplitude) / 16.0;
            segments.emplace_back(make_pair(
                (num_tick_samples * x) / this->opts->vibrato_resolution,
                effective_period * pow(2, -amplitude / 12.0)));
          }

        } else if (track.arpeggio_arg) {
          uint16_t periods[3] = {
            effective_period,
            static_cast<uint16_t>(
              effective_period / pow(2, ((track.arpeggio_arg >> 4) & 0x0F) / 12.0)),
            static_cast<uint16_t>(
              effective_period / pow(2, (track.arpeggio_arg & 0x0F) / 12.0)),
          };

          // The spec describes arpeggio effects as being "evenly spaced" within
          // the division, but some trackers (e.g. PlayerPRO) do not implement
          // this - instead, they simply iterate through the arpeggio periods
          // for each tick, and if the number of ticks per division isn't
          // divisible by 3, then some periods are held for longer. This
          // actually sounds better for some MODs, so we implement both this
          // behavior and true evenly-spaced arpeggio.
          if (this->opts->arpeggio_frequency <= 0) {
            for (size_t x = 0; x < timing.ticks_per_division; x++) {
              segments.emplace_back(make_pair(x * num_tick_samples, periods[x % 3]));
            }

          } else {
            // We multiply by 2 here since this is relative to the number of
            // output samples generated, and the output is stereo.
            size_t interval_samples =
                2 * timing.samples_per_tick * timing.ticks_per_division;

            // An arpeggio effect causes three fluctuations in the order
            // (note, note+x, note+y), a total of arpeggio_frequency times. The
            // intervals are evenly spaced across the division, independent of
            // tick boundaries.
            size_t denom = this->opts->arpeggio_frequency * 3;
            for (size_t x = 0; x < this->opts->arpeggio_frequency; x++) {
              segments.emplace_back(make_pair((3 * x + 0) * interval_samples / denom, periods[0]));
              segments.emplace_back(make_pair((3 * x + 1) * interval_samples / denom, periods[1]));
              segments.emplace_back(make_pair((3 * x + 2) * interval_samples / denom, periods[2]));
            }
          }

        } else {
          // If neither arpeggio nor vibrato happens in this tick, then the
          // period is effectively constant.
          segments.emplace_back(make_pair(0, effective_period));
        }

        // Figure out the volume for this tick.
        int8_t effective_volume = track.volume;
        if (track.tremolo_amplitude && track.tremolo_cycles) {
          effective_volume = this->get_vibrato_tremolo_wave_amplitude(
              track.tremolo_offset + static_cast<float>(track.tremolo_cycles) / 64, track.tremolo_waveform) * track.tremolo_amplitude;
          if (effective_volume < 0) {
            effective_volume = 0;
          } else if (effective_volume > 64) {
            effective_volume = 64;
          }
        }
        float track_volume_factor = static_cast<float>(effective_volume) / 64.0;
        float ins_volume_factor = static_cast<float>(i.volume) / 64.0;

        // Apply the appropriate portion of the instrument's sample data to the
        // tick output data.
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
            // The input samples to be played per second is:
            // track_input_samples_per_second = hardware_freq / (2 * period)
            // To convert this to the number of output samples per input sample,
            // all we have to do is divide the output sample rate by it:
            // out_samples_per_in_sample = sample_rate / (hardware_freq / (2 * period))
            // out_samples_per_in_sample = (sample_rate * 2 * period) / hardware_freq
            // This gives how many samples to generate for each input sample.
            src_ratio =
                static_cast<double>(2 * this->timing.sample_rate * segment.second) / this->opts->amiga_hardware_frequency;
            resampled_data = &this->sample_cache.resample_add(
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
              track.input_sample_offset = loop_start_offset / src_ratio;
            } else {
              track.input_sample_offset = i.sample_data.size();
            }
            break;
          }

          // When a new sample is played on a track and it interrupts another
          // already-playing sample, the waveform can become discontinuous,
          // which causes an audible ticking sound. To avoid this, we store a
          // DC offset in each track and adjust it so that the new sample begins
          // at the same amplitude. The DC offset then decays after each
          // subsequent sample and fairly quickly reaches zero. This eliminates
          // the tick and doesn't leave any other audible effects.
          float sample_from_ins = resampled_data->at(static_cast<size_t>(resampled_offset)) *
                track_volume_factor * ins_volume_factor;
          if (track.next_sample_may_be_discontinuous) {
            track.dc_offset -= sample_from_ins;
            track.last_sample = track.dc_offset;
            track.next_sample_may_be_discontinuous = false;
          } else {
            track.last_sample = sample_from_ins + track.dc_offset;
          }
          track.decay_dc_offset(this->dc_offset_decay);

          // Apply panning and produce the final sample. The surround effect
          // (enabled with effect 8A4) plays the same sample in both ears, but
          // with one inverted.
          float l_factor, r_factor;
          if (track.enable_surround_effect) {
            // TODO: is half volume correct here, or should we use full volume
            // on both ears?
            l_factor = (track.index & 1) ? -0.5 : 0.5;
            r_factor = (track.index & 1) ? 0.5 : -0.5;
          } else {
            l_factor = (1.0 - static_cast<float>(track.panning) / 128.0);
            r_factor = (static_cast<float>(track.panning) / 128.0);
          }
          tick_samples[tick_output_offset + 0] +=
              track.last_sample * l_factor * this->opts->global_volume;
          tick_samples[tick_output_offset + 1] +=
              track.last_sample * r_factor * this->opts->global_volume;

          // The observational spec claims that the loop only begins after the
          // the sample has been played to the end once, but this seems false.
          // It seems like we should instead always jump back when we reach the
          // end of the loop region, even the first time we reach it (which is
          // what's implemented here).
          resampled_offset++;
          // Since we use floats to represent the loop points, we actually could
          // miss it and think the sample ended when there's really a loop to be
          // played! To handle this, we assume that if we reach the end and a
          // loop is defined, we should just always use it.
          if ((loop_end_offset != 0.0) &&
              ((resampled_offset >= loop_end_offset) || (resampled_offset >= resampled_data->size() - 1))) {
            resampled_offset = loop_start_offset;
          } else if (resampled_offset >= resampled_data->size()) {
            track.input_sample_offset = i.sample_data.size();
            break;
          }

          // Advance the input offset by a proportional amount to the sound we
          // just generated, so the next tick or segment will start at the right
          // place
          track.input_sample_offset = resampled_offset / src_ratio;
        }

        // Apparently per-tick slides don't happen after the last tick in the
        // division. (Why? Protracker bug?)
        if (tick_num != timing.ticks_per_division - 1) {
          if (track.per_tick_period_increment) {
            track.period += track.per_tick_period_increment;
            // If a slide to note effect (3) is underway, enforce the limit
            // given by the effect command
            if (track.slide_target_period &&
                (((track.per_tick_period_increment > 0) &&
                  (track.period > track.slide_target_period)) ||
                 ((track.per_tick_period_increment < 0) &&
                  (track.period < track.slide_target_period)))) {
              track.period = track.slide_target_period;
              track.per_tick_period_increment = 0;
              track.slide_target_period = 0;
            }
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
        }
        track.vibrato_offset += static_cast<float>(track.vibrato_cycles) / 64;
        if (track.vibrato_offset >= 1) {
          track.vibrato_offset -= 1;
        }
        track.tremolo_offset += static_cast<float>(track.tremolo_cycles) / 64;
        if (track.tremolo_offset >= 1) {
          track.tremolo_offset -= 1;
        }
      }
      this->total_output_samples += tick_samples.size();
      on_tick_samples_ready(move(tick_samples));

      if (this->exceeded_time_limit()) {
        break;
      }
    }

    // Clear division-scoped effects on all tracks
    for (auto& track : tracks) {
      track.reset_division_scoped_effects();
    }
  }

  void advance_division() {
    if (this->pattern_break_target >= 0 && this->partition_break_target >= 0) {
      this->partition_index = this->partition_break_target;
      this->division_index = this->pattern_break_target;
      this->partition_break_target = -1;
      this->pattern_break_target = -1;
      this->pattern_loop_start_index = 0;

    } else if (this->jump_to_pattern_loop_start) {
      this->division_index = this->pattern_loop_start_index;
      this->jump_to_pattern_loop_start = false;

    } else {
      this->division_index++;
      if (this->division_index >= 64) {
        this->division_index = 0;
        this->partition_index++;
        this->pattern_loop_start_index = 0;
      }
    }

    if (this->partition_index >= this->mod->partition_count) {
      return;
    }
    if (this->division_index >= 64) {
      throw runtime_error("pattern break opcode jumps past end of next pattern");
    }
    this->partitions_executed.at(this->partition_index) = true;
  }

  bool exceeded_time_limit() const {
    return this->max_output_samples &&
        (this->total_output_samples > this->max_output_samples);
  }

public:
  void run() {
    bool changed_partition = true;
    this->max_output_samples = this->opts->output_sample_rate * this->opts->max_output_seconds * 2;
    while (this->partition_index < this->mod->partition_count && !this->exceeded_time_limit()) {
      this->show_current_division(changed_partition);
      this->execute_current_division_commands();
      for (this->divisions_to_delay++;
           this->divisions_to_delay > 0;
           this->divisions_to_delay--) {
        this->render_current_division_audio();
      }
      uint8_t old_partition_index = this->partition_index;
      this->advance_division();
      changed_partition = (this->partition_index != old_partition_index);
    }
  }
};



class MODWriter : public MODSynthesizer {
protected:
  FILE* f;

public:
  MODWriter(shared_ptr<const Module> mod, shared_ptr<const Options> opts, FILE* f)
    : MODSynthesizer(mod, opts), f(f) { }

  virtual void on_tick_samples_ready(vector<float>&& samples) {
    fwrite(samples.data(), sizeof(samples[0]), samples.size(), this->f);
    fflush(this->f);
  }
};



class MODExporter : public MODSynthesizer {
protected:
  deque<vector<float>> tick_samples;
  vector<float> all_tick_samples;

public:
  MODExporter(shared_ptr<const Module> mod, shared_ptr<const Options> opts)
    : MODSynthesizer(mod, opts) { }

  virtual void on_tick_samples_ready(vector<float>&& samples) {
    this->tick_samples.emplace_back(move(samples));
  }

  const vector<float>& result() {
    if (this->all_tick_samples.empty()) {
      this->all_tick_samples.reserve(this->total_output_samples);
      for (const auto& s : this->tick_samples) {
        this->all_tick_samples.insert(
            this->all_tick_samples.end(), s.begin(), s.end());
      }
    }
    return this->all_tick_samples;
  }
};



class MODPlayer : public MODSynthesizer {
protected:
  AudioStream stream;

public:
  MODPlayer(
      shared_ptr<const Module> mod,
      shared_ptr<const Options> opts,
      size_t num_play_buffers)
    : MODSynthesizer(mod, opts),
      stream(this->opts->output_sample_rate, format_for_name("stereo-f32"),
        num_play_buffers) { }

  virtual void on_tick_samples_ready(vector<float>&& samples) {
    this->stream.check_buffers();
    this->stream.add_samples(samples.data(), samples.size());
  }

  void drain() {
    this->stream.wait();
  }
};



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

  fprintf(stderr, "Normalizing volume by %lg\n", max_amplitude);
  if (max_amplitude == 0.0f) {
    return;
  }
  for (float& sample : data) {
    sample /= max_amplitude;
  }
}



void print_usage() {
  fprintf(stderr, "\
\n\
modsynth - a synthesizer for Protracker/Soundtracker modules\n\
\n\
Usage:\n\
  modsynth --disassemble [options] input_filename\n\
    Generates a human-readable representation of the instruments and sequence\n\
    program from the module. Options:\n\
      --show-sample-data: Shows raw sample data in a hex/ASCII view.\n\
      --show-sample-saveforms: Shows sample waveforms vertically. If color is\n\
          enabled, possibly-clipped samples are highlighted in red.\n\
      --show-unused-patterns: Disassemble all patterns, even those that don\'t\n\
          appear in the partition table.\n\
\n\
  modsynth --disassemble-directory [options] directory_name\n\
    Disassembles all files in the given directory. Options are the same as for\n\
    --disassemble.\n\
\n\
  modsynth --export-instruments input_filename\n\
    Exports the instruments from the module. Each instrument has at most one\n\
    sample. Each sample is saved as <input_filename>_<instrument_number>.wav.\n\
    Samples are converted to 32-bit floating-point format during export.\n\
\n\
  modsynth --render [options] input_filename\n\
    Generates a rasterized version of the sequence. Saves the result as\n\
    <input_filename>.wav. Options for both --play and --render:\n\
      --sample-rate=N: Output audio at this sample rate (default 48000).\n\
      --volume=N: Set global volume to N (-1.0-1.0; default 1.0). Negative\n\
          volumes simply invert the output waveform; it will sound the same as\n\
          a positive volume but can be used for some advanced effects.\n\
      --time-limit=N: Stop generating audio after this many seconds have been\n\
          generated (unlimited by default).\n\
      --skip-partitions=N: Start at this offset in the partition table instead\n\
          of at the beginning.\n\
      --allow-backward-position-jump: Allow position jump effects (Bxx) to jump\n\
          to parts of the song that have already been played. These generally\n\
          result in infinite loops and are disallowed by default.\n\
      --solo-track=N: Mute all the tracks except this one. The first track is\n\
          numbered 0; most MODs have tracks 0-3. May be given multiple times.\n\
      --mute-track=N: Mute this track. May be given multiple times.\n\
      --tempo-bias=N: Speed up or slow down the song by this factor without\n\
          changing pitch (default 1.0). For example, 2.0 plays the song twice\n\
          as fast; 0.5 plays the song at half speed.\n\
      --pal-amiga: Use a slightly lower hardware frequency when computing note\n\
          pitches, which matches Amiga machines sold in Europe. The default is\n\
          to use the North American machines' frequency. (The difference is\n\
          essentially imperceptible.)\n\
      --arpeggio-frequency=N: Use a fixed arpeggio frequency instead of the\n\
          default behavior, which is to align arpeggio boundaries to ticks.\n\
      --vibrato-resolution=N: Evaluate vibrato effects this many times each\n\
          tick (default 1).\n\
    Options for --render only:\n\
      --skip-normalize: By default, modsynth will normalize the output so the\n\
          maximum sample amplitude is 1.0 or -1.0. This option skips that step,\n\
          so the output may contain samples with higher amplitudes.\n\
      --write-stdout: Instead of saving to a file, write raw float32 data to\n\
          stdout, which can be piped to audiocat --play --format=stereo-f32.\n\
          Generally only useful for debugging problems with --render that don't\n\
          occur when using --play.\n\
\n\
  modsynth --play [options] input_filename\n\
    Plays the sequence through the default audio device.\n\
    Most options to --render apply here too. Extra options for --play only:\n\
      --play-buffers=N: Generate this many ticks of audio ahead of the output\n\
          device (default 8). If audio is choppy, try increasing this value.\n\
\n\
Options for all usage modes:\n\
  --color/--no-color: Enables or disables the generation of color escape codes\n\
      for visualizing pattern and instrument data. By default, color escapes\n\
      are generated only if the output is to a terminal.\n\
  --show-loading-debug: Show debugging information when loading the file.\n\
\n");
}

int main(int argc, char** argv) {
  enum class Behavior {
    Disassemble,
    DisassembleDirectory,
    ExportInstruments,
    Render,
    Play,
  };

  Behavior behavior = Behavior::Disassemble;
  const char* input_filename = nullptr;
  size_t num_play_buffers = 8;
  bool use_default_color_flags = true;
  bool write_stdout = false;
  bool use_default_global_volume = false;
  bool normalize_after_render = true;
  shared_ptr<MODSynthesizer::Options> opts(new MODSynthesizer::Options());
  for (int x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "--disassemble")) {
      behavior = Behavior::Disassemble;
    } else if (!strcmp(argv[x], "--disassemble-directory")) {
      behavior = Behavior::DisassembleDirectory;
    } else if (!strcmp(argv[x], "--export-instruments")) {
      behavior = Behavior::ExportInstruments;
    } else if (!strcmp(argv[x], "--render")) {
      behavior = Behavior::Render;
      opts->resample_method = SRC_SINC_BEST_QUALITY;
    } else if (!strcmp(argv[x], "--play")) {
      behavior = Behavior::Play;
      opts->resample_method = SRC_LINEAR;

    } else if (!strcmp(argv[x], "--write-stdout")) {
      write_stdout = true;

    } else if (!strcmp(argv[x], "--no-color")) {
      opts->flags &= ~Flags::TerminalColor;
      use_default_color_flags = false;
    } else if (!strcmp(argv[x], "--color")) {
      opts->flags |= Flags::TerminalColor;
      use_default_color_flags = false;
    } else if (!strcmp(argv[x], "--show-sample-data")) {
      opts->flags |= Flags::ShowSampleData;
    } else if (!strcmp(argv[x], "--show-sample-waveforms")) {
      opts->flags |= Flags::ShowSampleWaveforms;
    } else if (!strcmp(argv[x], "--show-unused-patterns")) {
      opts->flags |= Flags::ShowUnusedPatterns;
    } else if (!strcmp(argv[x], "--show-loading-debug")) {
      opts->flags |= Flags::ShowLoadingDebug;

    } else if (!strncmp(argv[x], "--solo-track=", 13)) {
      opts->solo_tracks.emplace(atoi(&argv[x][13]));
    } else if (!strncmp(argv[x], "--mute-track=", 13)) {
      opts->mute_tracks.emplace(atoi(&argv[x][13]));

    } else if (!strcmp(argv[x], "--pal-amiga")) {
      opts->amiga_hardware_frequency = 7093789.2;
    } else if (!strncmp(argv[x], "--tempo-bias=", 13)) {
      opts->tempo_bias = atof(&argv[x][13]);
    } else if (!strncmp(argv[x], "--volume=", 9)) {
      use_default_global_volume = false;
      opts->global_volume = atof(&argv[x][9]);
      if (opts->global_volume > 1.0) {
        opts->global_volume = 1.0;
      } else if (opts->global_volume < -1.0) {
        opts->global_volume = -1.0;
      }
    } else if (!strncmp(argv[x], "--time-limit=", 13)) {
      opts->max_output_seconds = atof(&argv[x][13]);

    } else if (!strcmp(argv[x], "--skip-normalize")) {
      normalize_after_render = false;

    } else if (!strncmp(argv[x], "--arpeggio-frequency=", 21)) {
      opts->arpeggio_frequency = atoi(&argv[x][21]);
    } else if (!strncmp(argv[x], "--vibrato-resolution=", 21)) {
      opts->vibrato_resolution = atoi(&argv[x][21]);

    } else if (!strncmp(argv[x], "--skip-partitions=", 18)) {
      opts->skip_partitions = atoi(&argv[x][18]);
    } else if (!strcmp(argv[x], "--allow-backward-position-jump")) {
      opts->allow_backward_position_jump = true;
    } else if (!strncmp(argv[x], "--play-buffers=", 15)) {
      num_play_buffers = atoi(&argv[x][15]);
    } else if (!strncmp(argv[x], "--sample-rate=", 14)) {
      opts->output_sample_rate = atoi(&argv[x][14]);

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

  bool behavior_is_disassemble = ((behavior == Behavior::Disassemble) ||
      (behavior == Behavior::DisassembleDirectory));
  if (use_default_color_flags &&
      isatty(fileno(behavior_is_disassemble ? stdout : stderr))) {
    opts->flags |= Flags::TerminalColor;
  }
  if (use_default_global_volume) {
    opts->global_volume = (behavior == Behavior::Play) ? 0.5 : 1.0;
  }

  shared_ptr<Module> mod;
  if (behavior != Behavior::DisassembleDirectory) {
    mod = load_mod(input_filename, opts->flags);
  }

  switch (behavior) {
    case Behavior::Disassemble:
      // We don't call print_mod_text in this case because all the text is
      // contained in the disassembly
      disassemble_mod(stdout, mod, opts->flags);
      break;
    case Behavior::DisassembleDirectory: {
      auto files = list_directory(input_filename);
      size_t num_disassembled = 0;
      for (const auto& filename : files) {
        string path = string(input_filename) + "/" + filename;
        fprintf(stdout, "===== %s\n", path.c_str());

        shared_ptr<Module> mod;
        try {
          disassemble_mod(stdout, load_mod(path, opts->flags), opts->flags);
          fputc('\n', stdout);
        } catch (const exception& e) {
          fprintf(stdout, "Failed: %s\n\n", e.what());
        }

        num_disassembled++;
        fprintf(stderr, "... (%zu/%zu) %s\n", num_disassembled, files.size(), path.c_str());
      }
      break;
    }
    case Behavior::ExportInstruments:
      export_mod_instruments(mod, opts->output_sample_rate, input_filename);
      break;
    case Behavior::Render: {
      print_mod_text(stderr, mod);
      if (write_stdout) {
        MODWriter writer(mod, opts, stdout);
        writer.run();
      } else {
        string output_filename = string(input_filename) + ".wav";
        MODExporter exporter(mod, opts);
        fprintf(stderr, "Synthesis:\n");
        exporter.run();
        fprintf(stderr, "Assembling result\n");
        auto result = exporter.result();
        if (normalize_after_render) {
          normalize_amplitude(result);
        }
        fprintf(stderr, "... %s\n", output_filename.c_str());
        save_wav(output_filename.c_str(), result, opts->output_sample_rate, 2);
      }
      break;
    }
    case Behavior::Play: {
      print_mod_text(stderr, mod);
      init_al();
      MODPlayer player(mod, opts, num_play_buffers);
      fprintf(stderr, "Synthesis:\n");
      player.run();
      player.drain();
      exit_al();
      break;
    }
    default:
      throw logic_error("invalid behavior");
  }

  return 0;
}
