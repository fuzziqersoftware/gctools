#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <samplerate.h>

#include <map>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <string>
#include <unordered_map>

#include "wav.hh"
#include "aaf.hh"
#include "audio.hh"

using namespace std;



enum DebugFlag {
  ShowResampleEvents          = 0x0000000000000001,
  ShowNotesOn                 = 0x0000000000000002,
  ShowKeyPresses              = 0x0000000000000004,
  ShowUnknownPerfOptions      = 0x0000000000000008,
  ShowUnknownParamOptions     = 0x0000000000000010,
  ShowUnimplementedConditions = 0x0000000000000020,
  ShowShortStatus             = 0x0000000000000040,

  ColorField                  = 0x0000000000000080,
  ColorStatus                 = 0x0000000000000100,
  AllColorOptions             = 0x0000000000000180,

  Default                     = 0x0000000000000182,
};

uint64_t debug_flags = DebugFlag::Default;

int resample_method = SRC_SINC_BEST_QUALITY;

vector<float> resample(const vector<float>& input_samples, size_t num_channels,
    double src_ratio) {
  size_t input_frame_count = input_samples.size() / num_channels;
  size_t output_frame_count = (input_frame_count * src_ratio) + 1;
  size_t output_sample_count = output_frame_count * num_channels;

  vector<float> output_samples(output_sample_count, 0.0f);

  SRC_DATA data;
  data.data_in = const_cast<float*>(input_samples.data());
  data.data_out = const_cast<float*>(output_samples.data());
  data.input_frames = input_frame_count;
  data.output_frames = output_frame_count;
  data.input_frames_used = 0;
  data.output_frames_gen = 0;
  data.end_of_input = 0;
  data.src_ratio = src_ratio;

  int error = src_simple(&data, resample_method, num_channels);
  if (error) {
    throw runtime_error(string_printf("src_simple failed: %s",
        src_strerror(error)));
  }

  output_samples.resize(data.output_frames_gen * num_channels);
  return output_samples;
}



string name_for_note(uint8_t note) {
  if (note >= 0x80) {
    return "invalid-note";
  }
  const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A",
      "A#", "B"};
  return string_printf("%s%hhu", names[note % 12], note / 12);
}

uint8_t lower_c_note_for_note(uint8_t note) {
  return note - (note % 12);
}

double frequency_for_note(uint8_t note) {
  static const double freq_table[0x80] = {
    8.1757989156,     8.6619572180,     9.1770239974,     9.7227182413,
    10.3008611535,    10.9133822323,    11.5623257097,    12.2498573744,
    12.9782717994,    13.7500000000,    14.5676175474,    15.4338531643,
    16.3515978313,    17.3239144361,    18.3540479948,    19.4454364826,
    20.6017223071,    21.8267644646,    23.1246514195,    24.4997147489,
    25.9565435987,    27.5000000000,    29.1352350949,    30.8677063285,
    32.7031956626,    34.6478288721,    36.7080959897,    38.8908729653,
    41.2034446141,    43.6535289291,    46.2493028390,    48.9994294977,
    51.9130871975,    55.0000000000,    58.2704701898,    61.7354126570,
    65.4063913251,    69.2956577442,    73.4161919794,    77.7817459305,
    82.4068892282,    87.3070578583,    92.4986056779,    97.9988589954,
    103.8261743950,   110.0000000000,   116.5409403795,   123.4708253140,
    130.8127826503,   138.5913154884,   146.8323839587,   155.5634918610,
    164.8137784564,   174.6141157165,   184.9972113558,   195.9977179909,
    207.6523487900,   220.0000000000,   233.0818807590,   246.9416506281,
    261.6255653006,   277.1826309769,   293.6647679174,   311.1269837221,
    329.6275569129,   349.2282314330,   369.9944227116,   391.9954359817,
    415.3046975799,   440.0000000000,   466.1637615181,   493.8833012561,
    523.2511306012,   554.3652619537,   587.3295358348,   622.2539674442,
    659.2551138257,   698.4564628660,   739.9888454233,   783.9908719635,
    830.6093951599,   880.0000000000,   932.3275230362,   987.7666025122,
    1046.5022612024,  1108.7305239075,  1174.6590716696,  1244.5079348883,
    1318.5102276515,  1396.9129257320,  1479.9776908465,  1567.9817439270,
    1661.2187903198,  1760.0000000000,  1864.6550460724,  1975.5332050245,
    2093.0045224048,  2217.4610478150,  2349.3181433393,  2489.0158697766,
    2637.0204553030,  2793.8258514640,  2959.9553816931,  3135.9634878540,
    3322.4375806396,  3520.0000000000,  3729.3100921447,  3951.0664100490,
    4186.009044809,   4434.922095630,   4698.636286678,   4978.031739553,
    5274.040910605,   5587.651702928,   5919.910763386,   6271.926975708,
    6644.875161279,   7040.000000000,   7458.620234756,   7902.132834658,
    8372.0180896192,  8869.8441912599,  9397.2725733570,  9956.0634791066,
    10548.0818212118, 11175.3034058561, 11839.8215267723, 12543.8539514160,
  };
  if (note >= 0x80) {
    throw invalid_argument("note does not exist");
  }
  return freq_table[note];
}



void disassemble_set_perf(size_t opcode_offset, uint8_t opcode, uint8_t type,
    uint8_t data_type, int16_t value, uint8_t duration_flags,
    uint16_t duration) {

  string param_name;
  if (type == 0x00) {
    param_name = "volume";
  } else if (type == 0x01) {
    param_name = "pitch_bend";
  } else if (type == 0x02) {
    param_name = "reverb";
  } else if (type == 0x03) {
    param_name = "panning";
  } else {
    param_name = string_printf("[%02hhX]", type);
  }

  printf("%08zX: set_perf        %s=", opcode_offset, param_name.c_str());
  if (data_type == 4) {
    printf("0x%02hhX (u8)", static_cast<uint8_t>(value));
  } else if (data_type == 8) {
    printf("0x%02hhX (s8)", static_cast<int8_t>(value));
  } else if (data_type == 12) {
    printf("0x%04hX (s16)", static_cast<int16_t>(value));
  }
  if (duration_flags == 2) {
    printf(", duration=0x%02hhX", static_cast<uint8_t>(duration));
  } else if (duration == 3) {
    printf(", duration=0x%04hX", static_cast<uint16_t>(duration));
  }
  printf("\n");
}

void disassemble_stream(StringReader& r, int32_t default_bank = -1) {
  unordered_map<size_t, string> track_start_labels;

  static const unordered_map<uint8_t, const char*> register_opcode_names({
    {0x00, "mov     "},
    {0x01, "add     "},
    {0x02, "sub     "},
    {0x03, "cmp     "},
    {0x04, "mul     "},
    {0x05, "and     "},
    {0x06, "or      "},
    {0x07, "xor     "},
    {0x08, "rnd     "},
    {0x09, "shl     "},
    {0x0A, "shr     "},
  });

  if (default_bank >= 0) {
    printf("/* note: default bank is %" PRId32 " */\n", default_bank);
  }

  while (!r.eof()) {
    size_t opcode_offset = r.where();

    {
      auto label_it = track_start_labels.find(opcode_offset);
      if (label_it != track_start_labels.end()) {
        printf("%s:\n", label_it->second.c_str());
        track_start_labels.erase(label_it);
      }
    }

    string disassembly;

    uint8_t opcode = r.get_u8();
    if (opcode < 0x80) {
      uint8_t voice = r.get_u8(); // between 1 and 8 inclusive
      uint8_t vel = r.get_u8();
      string note_name = name_for_note(opcode);
      disassembly = string_printf("note            note=%s, voice=%hhu, vel=0x%02hhX",
          note_name.c_str(), voice, vel);
    } else switch (opcode) {
      case 0x80: {
        uint8_t wait_time = r.get_u8();
        disassembly = string_printf("wait            %hhu", wait_time);
        break;
      }
      case 0x88: {
        uint16_t wait_time = r.get_u16r();
        disassembly = string_printf("wait            %hu", wait_time);
        break;
      }

      case 0x81:
      case 0x82:
      case 0x83:
      case 0x84:
      case 0x85:
      case 0x86:
      case 0x87: {
        uint8_t voice = opcode & 7;
        disassembly = string_printf("voice_off       %hhu", voice);
        break;
      }

      case 0x94:
      case 0x96:
      case 0x97:
      case 0x98:
      case 0x9A:
      case 0x9B:
      case 0x9C:
      case 0x9E:
      case 0x9F: {
        uint8_t type = r.get_u8();
        uint8_t duration_flags = opcode & 0x03;
        uint8_t data_type = opcode & 0x0C;
        int16_t value = 0;
        uint16_t duration = 0;
        if (data_type == 4) {
          value = r.get_u8();
        } else if (data_type == 8) {
          value = r.get_s8();
        } else if (data_type == 12) {
          value = r.get_s16r();
        }
        if (duration_flags == 2) {
          duration = r.get_u8();
        } else if (duration == 3) {
          duration = r.get_u16r();
        }

        static const unordered_map<uint8_t, string> param_names({
          {0x00, "volume"},
          {0x01, "pitch_bend"},
          {0x02, "reverb"},
          {0x03, "panning"},
        });
        string param_name;
        try {
          param_name = param_names.at(type);
        } catch (const out_of_range&) {
          param_name=string_printf("[%02hhX]", type);
        }

        disassembly = string_printf("set_perf        %s=", param_name.c_str());
        if (data_type == 4) {
          disassembly += string_printf("0x%02hhX (u8)", static_cast<uint8_t>(value));
        } else if (data_type == 8) {
          disassembly += string_printf("0x%02hhX (s8)", static_cast<int8_t>(value));
        } else if (data_type == 12) {
          disassembly += string_printf("0x%04hX (s16)", static_cast<int16_t>(value));
        }
        if (duration_flags == 2) {
          disassembly += string_printf(", duration=0x%02hhX", static_cast<uint8_t>(duration));
        } else if (duration == 3) {
          disassembly += string_printf(", duration=0x%04hX", static_cast<uint16_t>(duration));
        }

        break;
      }

      case 0xA4:
      case 0xAC: {
        uint8_t param = r.get_u8();
        uint16_t value = (opcode & 0x08) ? r.get_u16r() : r.get_u8();

        // guess: 07 as pitch bend semitones seems to make sense - some seqs set
        // it to 0x0C (one octave) immediately before/after a pitch bend opcode
        static const unordered_map<uint8_t, string> param_names({
          {0x07, "pitch_bend_semitones"},
          {0x20, "bank"},
          {0x21, "insprog"},
        });
        string param_name;
        try {
          param_name = param_names.at(param);
        } catch (const out_of_range&) {
          param_name=string_printf("[%02hhX]", param);
        }

        string value_str = (opcode & 0x08) ? string_printf("0x%04hX", value) :
            string_printf("0x%02hhX", static_cast<uint8_t>(value));
        disassembly = string_printf("set_param       %s, %s",
            param_name.c_str(), value_str.c_str());
        break;
      }

      case 0xC1: {
        uint8_t track_id = r.get_u8();
        uint32_t offset = r.get_u24r();
        disassembly = string_printf("start_track     %hhu, offset=0x%" PRIX32,
            track_id, offset);
        track_start_labels.emplace(offset, string_printf("track_%02hhX_start",
            track_id));
        break;
      }

      case 0xC3:
      case 0xC4:
      case 0xC7:
      case 0xC8: {
        const char* opcode_name = (opcode > 0xC4) ? "jmp " : "call";
        string conditional_str = (opcode & 1) ? "" :
            string_printf("cond=0x%02hhX, ", r.get_u8());

        uint32_t offset = r.get_u24r();
        disassembly = string_printf("%s            %soffset=0x%" PRIX32,
            opcode_name, conditional_str.c_str(), offset);
        break;
      }

      case 0xC5:
        disassembly = "ret";
        break;

      case 0xC6: {
        string conditional_str = string_printf("cond=0x%02hhX", r.get_u8());
        disassembly = string_printf("ret             %s", conditional_str.c_str());
        break;
      }

      case 0xE7: {
        uint16_t arg = r.get_u16r();
        disassembly = string_printf("sync_gpu        0x%04hX", arg);
        break;
      }

      case 0xFD: {
        uint16_t pulse_rate = r.get_u16r();
        disassembly = string_printf("set_pulse_rate  %hu", pulse_rate);
        break;
      }

      case 0xFE: {
        uint16_t tempo = r.get_u16r();
        uint64_t usec_pqn = 60000000 / tempo;
        disassembly = string_printf("set_tempo       %hu /* usecs per quarter note = %"
            PRIu64 " */", tempo, usec_pqn);
        break;
      }

      case 0xFF: {
        disassembly = "end_track";
        break;
      }

      // everything below here are register opcodes

      case 0xD0:
      case 0xD1:
      case 0xD4:
      case 0xD5:
      case 0xD6:
      case 0xD7: {
        static const unordered_map<uint8_t, const char*> opcode_names({
          {0xD0, "read_port    "},
          {0xD1, "write_port   "},
          {0xD4, "write_port_pr"},
          {0xD5, "write_port_ch"},
          {0xD6, "read_port_pr "},
          {0xD7, "read_port_ch "},
        });
        uint8_t port = r.get_u8();
        uint8_t reg = r.get_u8();
        disassembly = string_printf("%s   r%hhu, %hhu", opcode_names.at(opcode),
            reg, port);
        break;
      }

      case 0xD2:
        disassembly = string_printf(".check_port_in 0x%hX", r.get_u16r());
        break;

      case 0xD3:
        disassembly = string_printf(".check_port_ex 0x%hX", r.get_u16r());
        break;

      case 0xD8: {
        uint8_t reg = r.get_u8();
        int16_t val = r.get_s16r();
        disassembly = string_printf("mov            r%hhu, 0x%hX", reg, val);
        break;
      }

      case 0xD9: {
        uint8_t op = r.get_u8();
        uint8_t dst_reg = r.get_u8();
        uint8_t src_reg = r.get_u8();

        const char* opcode_name = ".unknown";
        try {
          opcode_name = register_opcode_names.at(op);
        } catch (const out_of_range&) { }

        disassembly = string_printf("%s            r%hhu, r%hhu", opcode_name,
            dst_reg, src_reg);
        break;
      }

      case 0xDA: {
        uint8_t op = r.get_u8();
        uint8_t dst_reg = r.get_u8();
        int16_t val = r.get_s16r();

        const char* opcode_name = ".unknown";
        try {
          opcode_name = register_opcode_names.at(op);
        } catch (const out_of_range&) { }

        disassembly = string_printf("%s            r%hhu, 0x%hX", opcode_name,
            dst_reg, val);
        break;
      }

      // everything below here are unknown opcodes

      case 0xC2:
      case 0xCF:
      case 0xDB:
      case 0xE2:
      case 0xE3:
      case 0xF1:
      case 0xF4: {
        uint8_t param = r.get_u8();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%02hhX",
            opcode, param);
        break;
      }

      case 0xA0:
      case 0xA3:
      case 0xA5:
      case 0xA7:
      case 0xB8:
      case 0xCB:
      case 0xCC:
      case 0xE0:
      case 0xE6:
      case 0xF9: {
        uint16_t param = r.get_u16r();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%04hX",
            opcode, param);
        break;
      }

      case 0xAD:
      case 0xAF:
      case 0xDD:
      case 0xEF: {
        uint32_t param = r.get_u24r();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%06" PRIX32,
            opcode, param);
        break;
      }

      case 0xA9:
      case 0xAA:
      case 0xDF: {
        uint32_t param = r.get_u32r();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%08" PRIX32,
            opcode, param);
        break;
      }

      case 0xB1: {
        uint8_t param1 = r.get_u8();
        if (param1 == 0x40) {
          uint16_t param2 = r.get_u16r();
          disassembly = string_printf(".unknown        0x%02hhX, 0x%02hhX, 0x%04hX",
              opcode, param1, param2);
        } else if (param1 == 0x80) {
          uint32_t param2 = r.get_u32r();
          disassembly = string_printf(".unknown        0x%02hhX, 0x%02hhX, 0x%08" PRIX32,
              opcode, param1, param2);
        } else {
          disassembly = string_printf(".unknown        0x%02hhX, 0x%02hhX",
              opcode, param1);
        }
        break;
      }

      case 0xF0: {
        uint64_t result = 0;
        uint8_t b = r.get_u8();
        while (b & 0x80) {
          result = (result << 7) | (b & 0x7F);
          b = r.get_u8();
        }
        result |= b;

        disassembly = string_printf("wait            %" PRIu64, result);
        break;
      }

      default:
        disassembly = string_printf(".unknown        0x%02hhX", opcode);
    }

    if (disassembly.empty()) {
      throw runtime_error("disassembly failure");
    }

    size_t opcode_size = r.where() - opcode_offset;
    string data = r.pread(opcode_offset, opcode_size);
    string data_str;
    for (char ch : data) {
      data_str += string_printf("%02X ", static_cast<uint8_t>(ch));
    }
    data_str.resize(18, ' ');

    printf("%08zX: %s  %s\n", opcode_offset, data_str.c_str(), disassembly.c_str());
  }
}



class SampleCache {
public:
  SampleCache() = default;
  ~SampleCache() = default;

  const vector<float>& at(const Sound* s, float conversion_rate) {
    return this->cache.at(s).at(conversion_rate);
  }

  void add(const Sound* s, float conversion_rate, vector<float>&& data) {
    this->cache[s].emplace(conversion_rate, move(data));
  }

private:
  unordered_map<const Sound*, unordered_map<float, vector<float>>> cache;
};



class Voice {
public:
  Voice(size_t sample_rate, int8_t note, int8_t vel) : sample_rate(sample_rate),
      note(note), vel(vel), note_off_decay_total(this->sample_rate / 5),
      note_off_decay_remaining(-1) { }
  virtual ~Voice() = default;

  virtual vector<float> render(size_t count, float volume, float pitch_bend,
      float pitch_bend_semitone_range, float panning) = 0;

  void off() {
    // TODO: for now we use a constant release time of 1/5 second; we probably
    // should get this from the AAF somewhere but I don't know where
    this->note_off_decay_remaining = this->note_off_decay_total;
  }

  bool off_complete() const {
    return (this->note_off_decay_remaining == 0);
  }

  float advance_note_off_factor() {
    if (this->note_off_decay_remaining == 0) {
      return 0.0f;
    }
    if (this->note_off_decay_remaining > 0) {
      return static_cast<float>(this->note_off_decay_remaining--) /
          this->note_off_decay_total;
    }
    return 1.0f;
  }

  size_t sample_rate;
  int8_t note;
  int8_t vel;
  ssize_t note_off_decay_total;
  ssize_t note_off_decay_remaining;
};

class SineVoice : public Voice {
public:
  SineVoice(size_t sample_rate, int8_t note, int8_t vel) :
      Voice(sample_rate, note, vel), offset(0) { }
  virtual ~SineVoice() = default;

  virtual vector<float> render(size_t count, float volume, float pitch_bend,
      float pitch_bend_semitone_range, float panning) {
    // TODO: implement pitch bend somehow
    vector<float> data(count * 2, 0.0f);

    double frequency = frequency_for_note(this->note);
    float vel_factor = static_cast<float>(this->vel) / 0x7F;
    for (size_t x = 0; x < count; x++) {
      // panning is 0.0f (left) - 1.0f (right)
      float off_factor = this->advance_note_off_factor();
      data[2 * x + 0] = vel_factor * off_factor * (1.0f - panning) * volume * sin((2.0f * M_PI * frequency) / this->sample_rate * (x + this->offset));
      data[2 * x + 1] = vel_factor * off_factor * panning * volume * sin((2.0f * M_PI * frequency) / this->sample_rate * (x + this->offset));
    }
    this->offset += count;

    return data;
  }

  size_t offset;
};

class SampleVoice : public Voice {
public:
  SampleVoice(size_t sample_rate, shared_ptr<const SoundEnvironment> env,
      shared_ptr<SampleCache> cache, uint16_t bank_id, uint16_t instrument_id,
      int8_t note, int8_t vel) : Voice(sample_rate, note, vel),
      instrument_bank(&env->instrument_banks.at(bank_id)),
      instrument(&this->instrument_bank->id_to_instrument.at(instrument_id)),
      key_region(&this->instrument->region_for_key(note)),
      vel_region(&this->key_region->region_for_velocity(vel)), src_ratio(1.0f),
      offset(0), cache(cache) {

    if (!this->vel_region->sound) {
      throw out_of_range("instrument sound is missing");
    }
    if (this->vel_region->sound->num_channels != 1) {
      // TODO: this probably wouldn't be that hard to support
      throw invalid_argument(string_printf(
          "sampled sound is multi-channel: %s:%" PRIX32,
          this->vel_region->sound->source_filename.c_str(),
          this->vel_region->sound->source_offset));
    }
  }

  virtual ~SampleVoice() = default;

  const vector<float>& get_samples(float pitch_bend,
      float pitch_bend_semitone_range) {
    // stretch it out by the sample rate difference
    float sample_rate_factor = static_cast<float>(sample_rate) /
        static_cast<float>(this->vel_region->sound->sample_rate);

    // compress it so it's the right note
    int8_t base_note = this->vel_region->base_note;
    if (base_note < 0) {
      base_note = this->vel_region->sound->base_note;
    }
    float note_factor = frequency_for_note(base_note) /
        frequency_for_note(this->note);

    {
      float pitch_bend_factor = pow(2, (pitch_bend * pitch_bend_semitone_range) / 12.0);
      float new_src_ratio = note_factor * sample_rate_factor /
          (this->vel_region->freq_mult * pitch_bend_factor);
      this->loop_start_offset = this->vel_region->sound->loop_start * new_src_ratio;
      this->loop_end_offset = this->vel_region->sound->loop_end * new_src_ratio;
      this->offset = this->offset * (new_src_ratio / this->src_ratio);
      this->src_ratio = new_src_ratio;
    }

    try {
      return this->cache->at(this->vel_region->sound, this->src_ratio);
    } catch (const out_of_range&) {
      if (debug_flags & DebugFlag::ShowResampleEvents) {
        string key_low_str = name_for_note(this->key_region->key_low);
        string key_high_str = name_for_note(this->key_region->key_high);
        fprintf(stderr, "[%s:%" PRIX64 "] resampling note %02hhX in range "
            "[%02hhX,%02hhX] [%s,%s] (base %02hhX from %s) (%g), with freq_mult %g, from "
            "%zuHz to %zuHz (%g) with loop at [%zu,%zu]->[%zu,%zu] for an overall "
            "ratio of %g\n", this->vel_region->sound->source_filename.c_str(),
            this->vel_region->sound->sound_id, this->note,
            this->key_region->key_low, this->key_region->key_high,
            key_low_str.c_str(), key_high_str.c_str(), base_note,
            (this->vel_region->base_note == -1) ? "sample" : "vel region", note_factor,
            this->vel_region->freq_mult, this->vel_region->sound->sample_rate,
            this->sample_rate, sample_rate_factor,
            this->vel_region->sound->loop_start, this->vel_region->sound->loop_end,
            this->loop_start_offset, this->loop_end_offset, this->src_ratio);
      }

      auto samples = resample(this->vel_region->sound->samples(),
          this->vel_region->sound->num_channels, this->src_ratio);
      this->cache->add(this->vel_region->sound, this->src_ratio, move(samples));
      return this->cache->at(this->vel_region->sound, this->src_ratio);
    }
  }

  virtual vector<float> render(size_t count, float volume, float pitch_bend,
      float pitch_bend_semitone_range, float panning) {
    vector<float> data(count * 2, 0.0f);

    const auto& samples = this->get_samples(pitch_bend, pitch_bend_semitone_range);
    float vel_factor = static_cast<float>(this->vel) / 0x7F;
    for (size_t x = 0; (x < count) && (this->offset < samples.size()); x++) {
      float off_factor = this->advance_note_off_factor();
      data[2 * x + 0] = vel_factor * off_factor * (1.0f - panning) * volume * samples[this->offset];
      data[2 * x + 1] = vel_factor * off_factor * panning * volume * samples[this->offset];

      this->offset++;
      if ((this->loop_end_offset > 0) && (this->offset > this->loop_end_offset)) {
        this->offset = this->loop_start_offset;
      }
    }
    if (this->offset == samples.size()) {
      this->note_off_decay_remaining = 0;
    }

    return data;
  }

  const InstrumentBank* instrument_bank;
  const Instrument* instrument;
  const KeyRegion* key_region;
  const VelocityRegion* vel_region;
  float src_ratio;

  size_t loop_start_offset;
  size_t loop_end_offset;
  size_t offset;

  shared_ptr<SampleCache> cache;
};

class Renderer {
private:
  struct Track {
    int16_t id;
    StringReader r;

    float volume;
    float volume_target;
    uint16_t volume_target_frames;

    float pitch_bend;
    float pitch_bend_target;
    uint16_t pitch_bend_target_frames;

    float reverb;
    float reverb_target;
    uint16_t reverb_target_frames;

    float panning;
    float panning_target;
    uint16_t panning_target_frames;

    int32_t bank; // technically uint16, but uninitialized as -1
    int32_t instrument; // technically uint16, but uninitialized as -1
    float pitch_bend_semitone_range;

    shared_ptr<Voice> voices[8];
    unordered_set<shared_ptr<Voice>> voices_off;
    vector<uint32_t> call_stack;

    unordered_map<uint8_t, int16_t> registers;

    Track(int16_t id, shared_ptr<string> data, size_t start_offset, uint32_t bank = -1) :
        id(id), r(data, start_offset), volume(0), volume_target(0),
        volume_target_frames(0), pitch_bend(0), pitch_bend_target(0),
        pitch_bend_target_frames(0), reverb(0), reverb_target(0),
        reverb_target_frames(0), panning(0.5f), panning_target(0.5f),
        panning_target_frames(0), bank(bank), instrument(-1),
        pitch_bend_semitone_range(48.0) {
      for (size_t x = 0; x < 8; x++) {
        this->voices[x].reset();
      }
    }

    void attenuate_perf() {
      if (this->volume_target_frames) {
        this->volume += (this->volume_target - this->volume) / this->volume_target_frames;
        this->volume_target_frames--;
      }
      if (this->pitch_bend_target_frames) {
        this->pitch_bend += (this->pitch_bend_target - this->pitch_bend) / this->pitch_bend_target_frames;
        this->pitch_bend_target_frames--;
      }
      if (this->reverb_target_frames) {
        this->reverb += (this->reverb_target - this->reverb) / this->reverb_target_frames;
        this->reverb_target_frames--;
      }
      if (this->panning_target_frames) {
        this->panning += (this->panning_target - this->panning) / this->panning_target_frames;
        this->panning_target_frames--;
      }
    }
  };

  shared_ptr<SequenceProgram> seq;
  shared_ptr<string> seq_data;
  string output_data;
  unordered_map<int16_t, shared_ptr<Track>> id_to_track;
  multimap<uint64_t, shared_ptr<Track>> next_event_to_track;

  size_t sample_rate;
  uint64_t current_time;
  size_t samples_rendered;
  uint16_t tempo;
  uint16_t pulse_rate;

  shared_ptr<const SoundEnvironment> env;
  unordered_set<int16_t> mute_tracks;
  unordered_set<int16_t> disable_tracks;

  shared_ptr<SampleCache> cache;

public:
  explicit Renderer(shared_ptr<SequenceProgram> seq, size_t sample_rate,
      shared_ptr<const SoundEnvironment> env,
      const unordered_set<int16_t>& mute_tracks,
      const unordered_set<int16_t>& disable_tracks) : seq(seq),
      seq_data(new string(seq->data)), sample_rate(sample_rate),
      current_time(0), samples_rendered(0), tempo(0), pulse_rate(0), env(env),
      mute_tracks(mute_tracks), disable_tracks(disable_tracks),
      cache(new SampleCache()) {
    // the default track has a track id of -1; all others are uint8_t
    if (!this->disable_tracks.count(-1)) {
      shared_ptr<Track> default_track(new Track(-1, this->seq_data, 0, this->seq->index));
      id_to_track.emplace(default_track->id, default_track);
      next_event_to_track.emplace(0, default_track);
    }
  }

  ~Renderer() = default;

  vector<float> render_time_step(size_t queued_buffer_count = 0, size_t buffer_count = 0) {
    if (this->next_event_to_track.empty()) {
      return vector<float>();
    }

    // run all opcodes that should execute on the current time step
    while (!this->next_event_to_track.empty() &&
           (current_time == this->next_event_to_track.begin()->first)) {
      this->execute_opcode(this->next_event_to_track.begin());
    }

    // figure out how many samples to produce
    if (this->sample_rate == 0) {
      throw invalid_argument("sample rate not set before producing audio");
    }
    if (this->tempo == 0) {
      throw invalid_argument("tempo not set before producing audio");
    }
    if (this->pulse_rate == 0) {
      throw invalid_argument("pulse rate not set before producing audio");
    }
    uint64_t usecs_per_qnote = 60000000 / this->tempo;
    double usecs_per_pulse = static_cast<double>(usecs_per_qnote) / this->pulse_rate;
    size_t samples_per_pulse = (usecs_per_pulse * this->sample_rate) / 1000000;

    // render this timestep
    vector<float> step_samples(2 * samples_per_pulse, 0);
    char notes_table[0x81];
    memset(notes_table, ' ', 0x80);
    notes_table[0x80] = 0;
    for (const auto& track_it : this->id_to_track) {
      shared_ptr<Track> t = track_it.second;

      // get all voices, including those that are fading
      unordered_set<shared_ptr<Voice>> all_voices = t->voices_off;
      for (size_t x = 0; x < 8; x++) {
        if (!t->voices[x].get()) {
          continue;
        }
        all_voices.insert(t->voices[x]);
      }

      // render all the voices
      for (auto v : all_voices) {
        vector<float> voice_samples = v->render(samples_per_pulse,
            t->volume, t->pitch_bend, t->pitch_bend_semitone_range, t->panning);
        if (voice_samples.size() != step_samples.size()) {
          throw logic_error(string_printf(
              "voice produced incorrect sample count (returned %zu samples, expected %zu samples)",
              voice_samples.size(), step_samples.size()));
        }
        if (!this->mute_tracks.count(track_it.first)) {
          for (size_t y = 0; y < voice_samples.size(); y++) {
            step_samples[y] += voice_samples[y];
          }
        }

        // only draw the note in the text view if it's on
        if (v->note_off_decay_remaining < 0) {
          int8_t note = v->note;
          char track_char = (t->id < 10) ? ('0' + t->id) : ('A' + t->id - 10);
          if (notes_table[note] == track_char) {
            continue;
          }
          if (notes_table[note] == ' ') {
            notes_table[note] = track_char;
          } else {
            notes_table[note] = '+';
          }
        }
      }

      // attenuate off voices and delete those that are fully off
      for (auto it = t->voices_off.begin(); it != t->voices_off.end();) {
        if ((*it)->off_complete()) {
          it = t->voices_off.erase(it);
        } else {
          it++;
        }
      }

      // attenuate the perf parameters
      t->attenuate_perf();
    }

    static const string field_red = format_color_escape(TerminalFormat::FG_RED, TerminalFormat::BOLD, TerminalFormat::END);
    static const string field_green = format_color_escape(TerminalFormat::FG_GREEN, TerminalFormat::BOLD, TerminalFormat::END);
    static const string field_yellow = format_color_escape(TerminalFormat::FG_YELLOW, TerminalFormat::BOLD, TerminalFormat::END);
    static const string field_blue = format_color_escape(TerminalFormat::FG_BLUE, TerminalFormat::BOLD, TerminalFormat::END);
    static const string field_magenta = format_color_escape(TerminalFormat::FG_MAGENTA, TerminalFormat::BOLD, TerminalFormat::END);
    static const string field_cyan = format_color_escape(TerminalFormat::FG_CYAN, TerminalFormat::BOLD, TerminalFormat::END);
    static const string green = format_color_escape(TerminalFormat::FG_GREEN, TerminalFormat::BOLD, TerminalFormat::END);
    static const string yellow = format_color_escape(TerminalFormat::FG_YELLOW, TerminalFormat::BOLD, TerminalFormat::END);
    static const string red = format_color_escape(TerminalFormat::FG_RED, TerminalFormat::BOLD, TerminalFormat::END);
    static const string white = format_color_escape(TerminalFormat::NORMAL, TerminalFormat::END);

    // render the text view
    if (debug_flags & DebugFlag::ShowNotesOn) {
      double when = static_cast<double>(this->samples_rendered) / this->sample_rate;

      const string* buffers_color;
      if (queued_buffer_count > (2 * buffer_count) / 3) {
        buffers_color = &green;
      } else if (queued_buffer_count > buffer_count / 3) {
        buffers_color = &yellow;
      } else {
        buffers_color = &red;
      }

      bool short_status = debug_flags & DebugFlag::ShowShortStatus;

      if ((debug_flags & DebugFlag::ColorField) || (short_status && (debug_flags & DebugFlag::ColorStatus))) {
        fprintf(stderr, "\r%08" PRIX64 ": %s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.8s%s @ %-7g + %s%zu/%zu%s%c",
            current_time,
            field_magenta.c_str(), &notes_table[0], field_red.c_str(), &notes_table[12],
            field_yellow.c_str(), &notes_table[24], field_green.c_str(), &notes_table[36],
            field_cyan.c_str(), &notes_table[48], field_blue.c_str(), &notes_table[60],
            field_magenta.c_str(), &notes_table[72], field_red.c_str(), &notes_table[84],
            field_yellow.c_str(), &notes_table[96], field_green.c_str(), &notes_table[108],
            field_cyan.c_str(), &notes_table[120], white.c_str(), when,
            buffers_color->c_str(), queued_buffer_count, buffer_count, white.c_str(),
            short_status ? ' ' : '\n');
      } else if (debug_flags & DebugFlag::ColorStatus) {
        fprintf(stderr, "\r%08" PRIX64 ": %s @ %-7g + %s%zu/%zu%s%c", current_time,
            notes_table, when, buffers_color->c_str(), queued_buffer_count,
            buffer_count, white.c_str(), short_status ? ' ' : '\n');
      } else {
        fprintf(stderr, "\r%08" PRIX64 ": %s @ %-7g + %zu/%zu%c", current_time,
            notes_table, when, queued_buffer_count, buffer_count, short_status ? ' ' : '\n');
      }

      if (!short_status) {
        if (debug_flags & DebugFlag::ColorStatus) {
          fprintf(stderr, "TIMESTEP: %sC D EF G A B%sC D EF G A B%sC D EF G A B%sC "
              "D EF G A B%sC D EF G A B%sC D EF G A B%sC D EF G A B%sC D EF G A B%s"
              "C D EF G A B%sC D EF G A B%sC D EF G%s @ SECONDS + %sBUF%s",
              field_magenta.c_str(), field_red.c_str(), field_yellow.c_str(),
              field_green.c_str(), field_cyan.c_str(), field_blue.c_str(),
              field_magenta.c_str(), field_red.c_str(), field_yellow.c_str(),
              field_green.c_str(), field_cyan.c_str(), white.c_str(),
              buffers_color->c_str(), white.c_str());
        } else {
          fprintf(stderr, "TIMESTEP: C D EF G A BC D EF G A BC D EF G A BC D EF G"
              " A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC D EF G A BC "
              "D EF G A BC D EF G @ SECONDS + BUF");
        }
      }
    }

    // advance to the next time step
    this->current_time++;
    this->samples_rendered += step_samples.size() / 2;

    return step_samples;
  }

  vector<float> render_until(uint64_t time) {
    vector<float> samples;
    while (!this->next_event_to_track.empty() && (this->current_time < time)) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }

  vector<float> render_until_seconds(float seconds) {
    vector<float> samples;
    size_t target_size = seconds * this->sample_rate;
    while (!this->next_event_to_track.empty() && (this->samples_rendered < target_size)) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }

  vector<float> render_all() {
    vector<float> samples;
    while (!this->next_event_to_track.empty()) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }

private:

  void execute_set_perf(shared_ptr<Track> t, uint8_t type, float value,
      uint16_t duration) {
    if (duration) {
      if (type == 0x00) {
        t->volume_target = value;
        t->volume_target_frames = duration;
      } else if (type == 0x01) {
        t->pitch_bend_target = value;
        t->pitch_bend_target_frames = duration;
      } else if (type == 0x02) {
        t->reverb_target = value;
        t->reverb_target_frames = duration;
      } else if (type == 0x03) {
        t->panning_target = value;
        t->panning_target_frames = duration;
      } else {
        if (debug_flags & DebugFlag::ShowUnknownPerfOptions) {
          fprintf(stderr, "unknown perf type option: %02hhX (value=%g)\n", type,
              value);
        }
      }

    } else {
      if (type == 0x00) {
        t->volume = value;
        t->volume_target_frames = 0;
      } else if (type == 0x01) {
        t->pitch_bend = value;
        t->pitch_bend_target_frames = 0;
      } else if (type == 0x02) {
        t->reverb = value;
        t->reverb_target_frames = 0;
      } else if (type == 0x03) {
        t->panning = value;
        t->panning_target_frames = 0;
      } else {
        if (debug_flags & DebugFlag::ShowUnknownPerfOptions) {
          fprintf(stderr, "unknown perf type option: %02hhX (value=%g)\n", type,
              value);
        }
      }
    }
  }

  void execute_set_param(shared_ptr<Track> t, uint8_t param, uint16_t value) {
    if (param == 0x20) {
      t->bank = value;
    } else if (param == 0x21) {
      t->instrument = value;
    } else if (param == 0x07) {
      // it looks like bms uses the same range for pitch bending as midi, which
      // is [-0x2000, +0x2000), but we convert [-0x8000, +0x7FFF) into
      // [-1.0, +1.0) linearly. so to correct for this, multiply by 4 here
      // TODO: verify if this is actually correct
      t->pitch_bend_semitone_range = static_cast<float>(value) * 4.0;
    } else {
      if (debug_flags & DebugFlag::ShowUnknownParamOptions) {
        fprintf(stderr, "unknown param type option: %02hhX (value=%hu)\n",
            param, value);
      }
    }
  }

  void execute_opcode(multimap<uint64_t, shared_ptr<Track>>::iterator track_it) {
    shared_ptr<Track> t = track_it->second;

    uint8_t opcode = t->r.get_u8();
    if (opcode < 0x80) {
      // note: opcode is also the note
      uint8_t voice = t->r.get_u8() - 1; // between 1 and 8 inclusive
      uint8_t vel = t->r.get_u8();
      if (voice >= 0x08) {
        return; // throw invalid_argument(string_printf("[%zX] voice out of range: %02hhX", where, voice));
      }

      // figure out which sample to use
      if (this->env) {
        try {
          SampleVoice* v = new SampleVoice(this->sample_rate, this->env,
              this->cache, t->bank, t->instrument, opcode, vel);
          t->voices[voice].reset(v);
        } catch (const out_of_range& e) {
          fprintf(stderr, "warning: can\'t find sample (%s): bank=%" PRIX32
              " instrument=%" PRIX32 " key=%02hhX vel=%02hhX\n", e.what(),
              t->bank, t->instrument, opcode, vel);
          t->voices[voice].reset(new SineVoice(this->sample_rate, opcode, vel));
        }
      } else {
        t->voices[voice].reset(new SineVoice(this->sample_rate, opcode, vel));
      }

      return;
    }

    switch (opcode) {
      case 0x80:
      case 0x88: {
        uint16_t wait_time = (opcode & 0x08) ? t->r.get_u16r() : t->r.get_u8();
        uint64_t reactivation_time = this->current_time + wait_time;
        this->next_event_to_track.erase(track_it);
        this->next_event_to_track.emplace(reactivation_time, t);
        break;
      }

      case 0x81:
      case 0x82:
      case 0x83:
      case 0x84:
      case 0x85:
      case 0x86:
      case 0x87: {
        uint8_t voice = (opcode & 7) - 1;
        // some tracks do voice_off for nonexistent voices because of bad
        // looping; just do nothing
        if (t->voices[voice].get()) {
          t->voices[voice]->off();
          t->voices_off.emplace(move(t->voices[voice]));
        }
        break;
      }

      case 0x94:
      case 0x96:
      case 0x97:
      case 0x98:
      case 0x9A:
      case 0x9B:
      case 0x9C:
      case 0x9E:
      case 0x9F: {
        uint8_t type = t->r.get_u8();
        uint8_t duration_flags = opcode & 0x03;
        uint8_t data_type = opcode & 0x0C;
        float value = 0.0f;
        uint16_t duration = 0;
        if (data_type == 4) {
          value = static_cast<float>(t->r.get_u8()) / 0xFF;
        } else if (data_type == 8) {
          value = static_cast<float>(t->r.get_s8()) / 0x7F;
        } else if (data_type == 12) {
          value = static_cast<float>(t->r.get_s16r()) / 0x7FFF;
        }
        if (duration_flags == 2) {
          duration = t->r.get_u8();
        } else if (duration == 3) {
          duration = t->r.get_u16r();
        }

        this->execute_set_perf(t, type, value, duration);
        break;
      }

      case 0xA4:
      case 0xAC: {
        uint8_t param = t->r.get_u8();
        uint16_t value = (opcode & 0x08) ? t->r.get_u16r() : t->r.get_u8();
        this->execute_set_param(t, param, value);
        break;
      }

      case 0xC1: {
        uint8_t track_id = t->r.get_u8();
        uint32_t offset = t->r.get_u24r();
        if (offset >= t->r.size()) {
          throw invalid_argument(string_printf(
              "cannot start track at pc=0x%" PRIX32 " (from pc=0x%zX)",
              offset, t->r.where() - 5));
        }
        if (!this->disable_tracks.count(track_id)) {
          shared_ptr<Track> new_track(new Track(track_id, this->seq_data, offset, this->seq->index));
          auto emplace_ret = this->id_to_track.emplace(track_id, new_track);
          if (!emplace_ret.second) {
            throw invalid_argument("attempted to start track that already existed");
          }
          this->next_event_to_track.emplace(this->current_time, new_track);
        }
        break;
      }

      case 0xC3:
      case 0xC4:
      case 0xC7:
      case 0xC8: {
        bool is_call = (opcode <= 0xC4);
        bool is_conditional = !(opcode & 1);

        int16_t cond = is_conditional ? t->r.get_u8() : -1;
        uint32_t offset = t->r.get_u24r();

        if (offset >= t->r.size()) {
          throw invalid_argument(string_printf(
              "cannot jump to pc=0x%" PRIX32 " (from pc=0x%zX)", offset,
              t->r.where() - 5));
        }

        if (cond > 0) {
          if (debug_flags & DebugFlag::ShowUnimplementedConditions) {
            fprintf(stderr, "unimplemented condition: 0x%02hX\n", cond);
          }

        // TODO: we should actually check the condition here
        } else {
          if (is_call) {
            t->call_stack.emplace_back(t->r.where());
          }
          t->r.go(offset);
        }
        break;
      }

      case 0xC5:
      case 0xC6: {
        bool is_conditional = !(opcode & 1);
        int16_t cond = is_conditional ? t->r.get_u8() : -1;

        if (cond > 0) {
          if (debug_flags & DebugFlag::ShowUnimplementedConditions) {
            fprintf(stderr, "unimplemented condition: 0x%02hX\n", cond);
          }

        // TODO: we should actually check the condition here
        } else {
          if (t->call_stack.empty()) {
            throw invalid_argument("return executed with empty call stack");
          }
          t->r.go(t->call_stack.back());
          t->call_stack.pop_back();
        }
        break;
      }

      case 0xE7: { // sync_gpu; note: arookas writes this as "track init"
        t->r.get_u16r();
        // TODO: what should we do here? anything?
        break;
      }

      case 0xFD: {
        this->pulse_rate = t->r.get_u16r();
        break;
      }

      case 0xFE: {
        this->tempo = t->r.get_u16r();
        break;
      }

      case 0xFF: {
        this->id_to_track.erase(t->id);
        this->next_event_to_track.erase(track_it);
        break;
      }

      // everything below here are unknown opcodes

      case 0xE1:
      case 0xFA:
      case 0xBF:
        break;

      case 0xC2:
      case 0xCF:
      case 0xDA:
      case 0xDB:
      case 0xE2:
      case 0xE3:
      case 0xF1:
      case 0xF4:
        t->r.get_u8();
        break;

      case 0xD0:
      case 0xD1:
      case 0xD2:
      case 0xD5:
      case 0xA0:
      case 0xA3:
      case 0xA5:
      case 0xA7:
      case 0xB8:
      case 0xCB:
      case 0xCC:
      case 0xD6:
      case 0xE0:
      case 0xE6:
      case 0xF9:
        t->r.get_u16r();
        break;

      case 0xAD:
      case 0xAF:
      case 0xD8:
      case 0xDD:
      case 0xEF:
        t->r.get_u24r();
        break;

      case 0xA9:
      case 0xAA:
      case 0xDF:
        t->r.get_u32r();
        break;

      case 0xB1: {
        uint8_t param1 = t->r.get_u8();
        if (param1 == 0x40) {
          t->r.get_u16r();
        } else if (param1 == 0x80) {
          t->r.get_u32r();
        }
        break;
      }

      case 0xF0:
        while (t->r.get_u8() & 0x80);
        break;

      default:
        throw invalid_argument(string_printf("unknown opcode at offset 0x%zX: 0x%hhX",
            t->r.where() - 1, opcode));
    }
  }
};



void print_usage(const char* argv0) {
  fprintf(stderr, "\
usage:\n\
  to disassemble: %s sequence.bms [options]\n\
  to render: %s sequence.bms --output-filename=file.wav [options]\n\
  to play: %s sequence.bms --play [options]\n\
\n\
options:\n\
  --disable-track=N: disable track N completely.\n\
  --mute-track=N: execute track N, but don't render any of its sound.\n\
  --time-limit=N: stop playing or rendering after this many seconds.\n\
  --sample-rate=N: render or play at this sample rate.\n\
  --audiores-directory=DIR: AudioRes directory extracted from Sunshine disc.\n\
      if given, the bms filename argument may also be the name of a sequence\n\
      defined in the aaf file (the program will check for this first).\n\
      if not given, all instruments will be sine waves, which sounds funny but\n\
      probably isn\'t what you want.\n\
  --verbose: print debugging events and the like.\n\
  --linear: use linear interpolation. realtime play will likely lag unless this\n\
      option is used, especially if the sequence uses pitch bending.\n\
  --play-buffers=N: generate up to N steps ahead of playback at once\n\
  --default-bank=N: override automatic instrument bank detection and use bank\n\
      N instead.\n\
  --wsys-link=A:B: override automatic sample bank detection and use samples\n\
      from bank B for instrument bank A instead.\n\
  --no-color: don\'t use terminal escape codes for color in the output.\n\
  --short-status: only show one line of status information.\n\
", argv0, argv0, argv0);
}

int main(int argc, char** argv) {

  // default to no color if stderr isn't a tty
  if (!isatty(fileno(stderr))) {
    debug_flags &= ~DebugFlag::AllColorOptions;
  }

  const char* filename = NULL;
  const char* output_filename = NULL;
  const char* aaf_directory = NULL;
  unordered_set<int16_t> disable_tracks;
  unordered_set<int16_t> mute_tracks;
  float time_limit = 60.0f;
  float start_time = 0.0f;
  size_t sample_rate = 192000;
  bool play = false;
  int32_t default_bank = -1;
  unordered_map<uint32_t, uint32_t> wsys_link_overrides;
  size_t num_buffers = 32;
  for (int x = 1; x < argc; x++) {
    if (!strncmp(argv[x], "--disable-track=", 16)) {
      disable_tracks.emplace(atoi(&argv[x][16]));
    } else if (!strncmp(argv[x], "--mute-track=", 13)) {
      mute_tracks.emplace(atoi(&argv[x][13]));
    } else if (!strncmp(argv[x], "--time-limit=", 13)) {
      time_limit = atof(&argv[x][13]);
    } else if (!strncmp(argv[x], "--start-time=", 13)) {
      start_time = atof(&argv[x][13]);
    } else if (!strncmp(argv[x], "--sample-rate=", 14)) {
      sample_rate = atoi(&argv[x][14]);
    } else if (!strncmp(argv[x], "--audiores-directory=", 21)) {
      aaf_directory = &argv[x][21];
    } else if (!strncmp(argv[x], "--output-filename=", 18)) {
      output_filename = &argv[x][18];

    } else if (!strcmp(argv[x], "--verbose")) {
      debug_flags = 0xFFFFFFFFFFFFFFFF;
    } else if (!strncmp(argv[x], "--debug-flags=", 14)) {
      debug_flags = atoi(&argv[x][14]);
    } else if (!strcmp(argv[x], "--no-color")) {
      debug_flags &= ~DebugFlag::AllColorOptions;
    } else if (!strcmp(argv[x], "--short-status")) {
      debug_flags |= DebugFlag::ShowShortStatus;

    } else if (!strncmp(argv[x], "--linear", 8)) {
      resample_method = SRC_LINEAR;
    } else if (!strncmp(argv[x], "--default-bank=", 15)) {
      default_bank = atoi(&argv[x][15]);
    } else if (!strncmp(argv[x], "--wsys-link=", 12)) {
      uint32_t ibnk_id, wsys_id;
      sscanf(&argv[x][12], "%" PRIX32 ":%" PRIX32, &ibnk_id, &wsys_id);
      wsys_link_overrides.emplace(ibnk_id, wsys_id);
    } else if (!strcmp(argv[x], "--play")) {
      play = true;
    } else if (!strncmp(argv[x], "--play-buffers=", 15)) {
      num_buffers = atoi(&argv[x][15]);
    } else if (!filename) {
      filename = argv[x];
    } else {
      throw invalid_argument("too many positional command-line args");
    }
  }
  if (!filename) {
    print_usage(argv[0]);
    throw invalid_argument("no filename given");
  }

  shared_ptr<const SoundEnvironment> env(aaf_directory ?
      new SoundEnvironment(load_sound_environment(aaf_directory, wsys_link_overrides)) : NULL);

  // try to get the sequence from the env if it's there
  shared_ptr<SequenceProgram> seq;
  if (env.get()) {
    try {
      seq.reset(new SequenceProgram(env->sequence_programs.at(filename)));
    } catch (const out_of_range&) { }
  }
  if (!seq.get()) {
    try {
      seq.reset(new SequenceProgram(default_bank, load_file(filename)));
    } catch (const cannot_open_file&) {
      fprintf(stderr, "sequence does not exist in environment, nor on disk: %s\n",
          filename);
      fprintf(stderr, "there are %zu sequences in the environment:\n",
          env->sequence_programs.size());

      vector<string> sequence_names;
      for (const auto& it : env->sequence_programs) {
        sequence_names.emplace_back(it.first);
      }
      sort(sequence_names.begin(), sequence_names.end());

      for (const auto& it : sequence_names) {
        fprintf(stderr, "  %s\n", it.c_str());
      }
      return 2;
    }
  }

  if (default_bank >= 0) {
    seq->index = default_bank;
  }

  if (output_filename) {
    Renderer r(seq, sample_rate, env, mute_tracks, disable_tracks);

    if (start_time) {
      r.render_until_seconds(start_time);
    }

    auto samples = r.render_until_seconds(time_limit);
    save_wav(output_filename, samples, sample_rate, 2);

  } else if (play) {
    Renderer r(seq, sample_rate, env, mute_tracks, disable_tracks);

    if (start_time) {
      r.render_until_seconds(start_time);
    }

    init_al();
    al_stream stream(sample_rate, AL_FORMAT_STEREO16, num_buffers);
    for (;;) {
      stream.check_buffers();
      auto step_samples = r.render_time_step(stream.queued_buffer_count(), stream.buffer_count());
      if (step_samples.empty()) {
        break;
      }
      vector<int16_t> al_samples = convert_samples_to_int(step_samples);
      stream.add_samples(al_samples.data(), al_samples.size() / 2);
    }
    if (debug_flags & DebugFlag::ShowNotesOn) {
      fprintf(stderr, "\nrendering complete; waiting for buffers to drain\n");
    }
    stream.wait();
    exit_al();

  } else {
    shared_ptr<string> seq_data(new string(seq->data));
    StringReader r(seq_data);
    disassemble_stream(r, seq->index);
  }

  return 0;
}
