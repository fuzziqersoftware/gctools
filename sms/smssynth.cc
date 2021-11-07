#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <samplerate.h>

#include <algorithm>
#include <map>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <phosg-audio/Convert.hh>
#include <phosg-audio/File.hh>
#include <phosg-audio/Stream.hh>
#include <string>
#include <unordered_map>

#include "aaf.hh"
#include "SampleCache.hh"

#ifdef WINDOWS

#define PRId32 "d"
#define PRIu32 "u"
#define PRIX32 "X"
#define PRIu64 "llu"
#define PRIX64 "llX"

#endif

using namespace std;



enum DebugFlag {
  ShowResampleEvents          = 0x0000000000000001,
  ShowNotesOn                 = 0x0000000000000002,
  ShowKeyPresses              = 0x0000000000000004,
  ShowUnknownPerfOptions      = 0x0000000000000008,
  ShowUnknownParamOptions     = 0x0000000000000010,
  ShowUnimplementedConditions = 0x0000000000000020,
  ShowLongStatus              = 0x0000000000000040,
  ShowMissingNotes            = 0x0000000000000080,
  ShowUnimplementedOpcodes    = 0x0000000000000100,

  PlayMissingNotes            = 0x0000000000010000,

  ColorField                  = 0x0000000000020000,
  ColorStatus                 = 0x0000000000040000,
  AllColorOptions             = 0x0000000000060000,

#ifndef WINDOWS
  Default                     = 0x00000000000600C2,
#else
  // no color by default on windows (cmd.exe doesn't handle the escapes)
  Default                     = 0x00000000000000C2,
#endif
};

uint64_t debug_flags = DebugFlag::Default;



bool is_binary(const char* s, size_t size) {
  for (size_t x = 0; x < size; x++) {
    if ((s[x] < 0x20) || (s[x] > 0x7F)) {
      return true;
    }
  }
  return false;
}

bool is_binary(const string& s) {
  return is_binary(s.data(), s.size());
}

uint8_t lower_c_note_for_note(uint8_t note) {
  return note - (note % 12);
}



struct MIDIChunkHeader {
  uint32_t magic;
  uint32_t size;

  void byteswap() {
    this->magic = bswap32(this->magic);
    this->size = bswap32(this->size);
  }
} __attribute__((packed));

struct MIDIHeaderChunk {
  MIDIChunkHeader header; // magic=MThd, size=6
  uint16_t format; // 0, 1, or 2. see below
  uint16_t track_count;
  uint16_t division; // see below

  // format=0: file contains a single track
  // format=1: file contains simultaneous tracks (start them all at once)
  // format=2: file contains independent tracks

  // if the MSB of division is 1, then the remaining 15 bits are the number of
  // ticks per quarter note. if the MSB is 0, then the next 7 bits are
  // frames/second (as a negative number), and the last 8 are ticks per frame

  void byteswap() {
    this->header.byteswap();
    this->format = bswap16(this->format);
    this->track_count = bswap16(this->track_count);
    this->division = bswap16(this->division);
  }
} __attribute__((packed));

struct MIDITrackChunk {
  MIDIChunkHeader header;
  uint8_t data[0];

  void byteswap() {
    this->header.byteswap();
  }
} __attribute__((packed));



uint64_t read_variable_int(StringReader& r) {
  uint8_t b = r.get_u8();
  if (!(b & 0x80)) {
    return b;
  }

  uint64_t v = 0;
  while (b & 0x80) {
    v = (v << 7) | (b & 0x7F);
    b = r.get_u8();
  }
  return (v << 7) | b;
}



void disassemble_set_perf(
    size_t opcode_offset,
    uint8_t opcode,
    uint8_t type,
    uint8_t data_type,
    int16_t value,
    uint8_t duration_flags,
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

void disassemble_bms(StringReader& r, int32_t default_bank = -1) {
  unordered_map<size_t, string> track_start_labels;

  static const unordered_map<uint8_t, const char*> register_opcode_names({
    {0x00, "mov      "},
    {0x01, "add      "},
    {0x02, "sub      "},
    {0x03, "cmp      "},
    {0x04, "mul      "},
    {0x05, "and      "},
    {0x06, "or       "},
    {0x07, "xor      "},
    {0x08, "rnd      "},
    {0x09, "shl      "},
    {0x0A, "shr      "},
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

      case 0xE0:
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
        uint8_t value = r.get_u8();
        disassembly = string_printf("%s   r%hhu, %hhu, %hhu", opcode_names.at(opcode),
            reg, port, value);
        break;
      }

      case 0xD2:
        disassembly = string_printf(".check_port_in  0x%hX", r.get_u16r());
        break;

      case 0xD3:
        disassembly = string_printf(".check_port_ex  0x%hX", r.get_u16r());
        break;

      case 0xD8: {
        uint8_t reg = r.get_u8();
        int16_t val = r.get_s16r();
        if (reg == 0x62) {
          disassembly = string_printf("mov             r98, %hd /* set_pulse_rate */", val);
        } else {
          disassembly = string_printf("mov             r%hhu, 0x%hX", reg, val);
        }
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

        disassembly = string_printf("%s             r%hhu, r%hhu", opcode_name,
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

      case 0xE2:
        disassembly = string_printf("set_bank        0x%hX", r.get_u8());
        break;
      case 0xE3:
        disassembly = string_printf("set_instrument  0x%hX", r.get_u8());
        break;

      case 0xFB: {
        string s;
        char b;
        while ((b = r.get_u8())) {
          s.push_back(b);
        }
        disassembly = string_printf("debug_str       \"%s\"", s.c_str());
        break;
      }

      // everything below here are unknown opcodes

      case 0xC2:
      case 0xCD:
      case 0xCF:
      case 0xDB:
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
      case 0xE6:
      case 0xF9: {
        uint16_t param = r.get_u16r();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%04hX",
            opcode, param);
        break;
      }

      case 0xAD:
      case 0xAF:
      case 0xB9:
      case 0xDD:
      case 0xEF: {
        uint32_t param = r.get_u24r();
        disassembly = string_printf(".unknown        0x%02hhX, 0x%06" PRIX32,
            opcode, param);
        break;
      }

      case 0xA9:
      case 0xAA:
      case 0xB4:
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
        disassembly = string_printf("wait            %" PRIu64, read_variable_int(r));
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



void disassemble_midi(StringReader& r) {
  // read the header, check it, and disassemble it
  MIDIHeaderChunk header = r.get<MIDIHeaderChunk>();
  header.byteswap();
  if (header.header.magic != 0x4D546864) { // 'MThd'
    throw runtime_error("header identifier is incorrect");
  }
  if (header.header.size < 6) {
    throw runtime_error("header is too small");
  }
  if (header.format > 2) {
    throw runtime_error("MIDI format is unknown");
  }
  printf("# MIDI format %hu, %hu tracks, division %04X\n", header.format,
      header.track_count, header.division);

  // if the header is larger, skip the extra bytes
  if (header.header.size > 6) {
    r.go(r.where() + (header.header.size - sizeof(MIDIHeaderChunk)));
  }

  // disassemble each track
  for (size_t track_id = 0; track_id < header.track_count; track_id++) {
    size_t header_offset = r.where();
    MIDITrackChunk ch = r.get<MIDITrackChunk>();
    ch.byteswap();
    if (ch.header.magic != 0x4D54726B) {
      throw runtime_error("track header not present");
    }

    printf("Track %zu:  # header_offset=0x%zX\n", track_id, header_offset);

    size_t end_offset = r.where() + ch.header.size;
    uint8_t status = 0;
    while (r.where() < end_offset) {
      size_t event_offset = r.where();
      uint32_t wait_ticks = read_variable_int(r);
      if (wait_ticks) {
        printf("%08zX  +%-7d  ", event_offset, wait_ticks);
      } else {
        printf("%08zX            ", event_offset);
      }

      // if the status byte is omitted, it uses the status from the previous
      // command
      uint8_t new_status = r.get_u8();
      if (new_status & 0x80) {
        status = new_status;
      } else {
        r.go(r.where() - 1);
      }

      if ((status & 0xF0) == 0x80) { // note off
        uint8_t channel = status & 0x0F;
        uint8_t key = r.get_u8();
        uint8_t vel = r.get_u8();
        string note = name_for_note(key);
        printf("note_off     channel%hhu, %s, %hhu\n", channel, note.c_str(),
            vel);

      } else if ((status & 0xF0) == 0x90) { // note on
        uint8_t channel = status & 0x0F;
        uint8_t key = r.get_u8();
        uint8_t vel = r.get_u8();
        string note = name_for_note(key);
        printf("note_on      channel%hhu, %s, %hhu\n", channel, note.c_str(),
            vel);

      } else if ((status & 0xF0) == 0xA0) { // change key pressure
        uint8_t channel = status & 0x0F;
        uint8_t key = r.get_u8();
        uint8_t vel = r.get_u8();
        string note = name_for_note(key);
        printf("change_vel   channel%hhu, %s, %hhu\n", channel, note.c_str(),
            vel);

      } else if ((status & 0xF0) == 0xB0) { // controller change OR channel mode
        uint8_t channel = status & 0x0F;
        uint8_t controller = r.get_u8();
        uint8_t value = r.get_u8();
        if (controller == 0x07) {
          printf("volume       channel%hhu, 0x%02hhX\n", channel, value);
        } else if (controller == 0x0A) {
          printf("panning      channel%hhu, 0x%02hhX\n", channel, value);
        } else if (controller == 0x78) {
          printf("mute_all     channel%hhu\n", channel);
        } else if (controller == 0x79) {
          printf("reset_all    channel%hhu\n", channel);
        } else if (controller == 0x7A) {
          printf("local_ctrl   channel%hhu, %s\n", channel,
              value ? "on" : "off");
        } else if (controller == 0x7B) {
          printf("note_off_all channel%hhu\n", channel);
        } else if (controller == 0x7C) {
          printf("omni_off     channel%hhu\n", channel);
        } else if (controller == 0x7D) {
          printf("omni_on      channel%hhu\n", channel);
        } else if (controller == 0x7D) {
          printf("mono         channel%hhu, %hhu\n", channel, value);
        } else if (controller == 0x7D) {
          printf("poly         channel%hhu\n", channel);
        } else {
          printf("controller   channel%hhu, 0x%02hhX, 0x%02hhX\n", channel, controller,
              value);
        }

      } else if ((status & 0xF0) == 0xC0) { // program change
        uint8_t channel = status & 0x0F;
        uint8_t program_number = r.get_u8();
        printf("change_prog  channel%hhu, %hhu\n", channel, program_number);

      } else if ((status & 0xF0) == 0xD0) { // channel key pressure
        uint8_t channel = status & 0x0F;
        uint8_t vel = r.get_u8();
        printf("change_vel   channel%hhu, %hhu\n", channel, vel);

      } else if ((status & 0xF0) == 0xE0) { // pitch bend
        uint8_t channel = status & 0x0F;
        uint8_t lsb = r.get_u8();
        uint8_t msb = r.get_u8();
        uint16_t value = (msb << 7) | lsb; // yes, each is 7 bits, not 8
        printf("pitch_bend   channel%hhu, %hu\n", channel, value);

      } else if (status == 0xFF) { // meta event
        uint8_t type = r.get_u8();
        uint8_t size = r.get_u8();

        if ((type == 0x00) && (size == 0x02)) {
          printf("seq_number   %hu\n", r.get_u16r());

        } else if (type == 0x01) {
          string data = r.read(size);
          if (is_binary(data)) {
            string data_str = format_data_string(data);
            printf("text         0x%s\n", data_str.c_str());
          } else {
            printf("text         \"%s\"\n", data.c_str());
          }

        } else if (type == 0x02) {
          string data = r.read(size);
          printf("copyright    \"%s\"\n", data.c_str());

        } else if (type == 0x03) {
          string data = r.read(size);
          printf("name         \"%s\"\n", data.c_str());

        } else if (type == 0x04) {
          string data = r.read(size);
          printf("ins_name     \"%s\"\n", data.c_str());

        } else if (type == 0x05) {
          string data = r.read(size);
          printf("lyric        \"%s\"\n", data.c_str());

        } else if (type == 0x06) {
          string data = r.read(size);
          printf("marker       \"%s\"\n", data.c_str());

        } else if (type == 0x07) {
          string data = r.read(size);
          printf("cue_point    \"%s\"\n", data.c_str());

        } else if ((type == 0x20) && (size == 1)) {
          uint8_t channel = r.get_u8();
          printf("channel_pfx  channel%hhu\n", channel);

        } else if ((type == 0x2F) && (size == 0)) {
          printf("end_track\n");

        } else if ((type == 0x51) && (size == 3)) {
          uint32_t usecs_per_qnote = r.get_u24r();
          printf("set_tempo    %" PRIu32 "\n", usecs_per_qnote);

        } else if ((type == 0x54) && (size == 5)) {
          uint8_t hours = r.get_u8();
          uint8_t minutes = r.get_u8();
          uint8_t seconds = r.get_u8();
          uint8_t frames = r.get_u8();
          uint8_t frame_fraction = r.get_u8();
          printf("set_offset   %02hhu:%02hhu:%02hhu#%02hhu.%02hhu\n", hours,
              minutes, seconds, frames, frame_fraction);

        } else if ((type == 0x58) && (size == 4)) {
          uint8_t numer = r.get_u8();
          uint8_t denom = r.get_u8();
          uint8_t ticks_per_metronome_tick = r.get_u8();
          uint8_t b = r.get_u8(); // 1/32 notes per 24 midi ticks
          printf("time_sig     %02hhu:%02hhu, midi_ticks=%02hhu, ratio=%hhu\n",
              numer, denom, ticks_per_metronome_tick, b);

        } else if ((type == 0x59) && (size == 2)) {
          uint8_t sharps = r.get_u8();
          uint8_t major = r.get_u8();
          printf("key_sig      sharps=%02hhu, %s\n", sharps,
              major ? "major" : "minor");

        } else if (size) { // unknown meta with data
          string data = format_data_string(r.read(size));
          printf(".meta        0x%hhX, %s\n", type, data.c_str());
        } else { // unknown meta without data
          printf(".meta        0x%hhX\n", type);
        }

      } else {
        throw runtime_error(string_printf("invalid status byte: %02hhX", status));
      }
    }

    if (r.where() != end_offset) {
      throw runtime_error("track end is misaligned");
    }
  }
}



struct Channel {
  float pitch_bend_semitone_range;

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

  Channel() : pitch_bend_semitone_range(48.0), volume(1.0), volume_target(0),
      volume_target_frames(0), pitch_bend(0), pitch_bend_target(0),
      pitch_bend_target_frames(0), reverb(0), reverb_target(0),
      reverb_target_frames(0), panning(0.5f), panning_target(0.5f),
      panning_target_frames(0) { }

  void attenuate() {
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



class Voice {
public:
  Voice(size_t sample_rate, int8_t note, int8_t vel, bool decay_when_off,
      shared_ptr<Channel> channel) : sample_rate(sample_rate), note(note),
      vel(vel), channel(channel), decay_when_off(decay_when_off),
      note_off_decay_total(this->sample_rate / 5),
      note_off_decay_remaining(-1) { }
  virtual ~Voice() = default;

  virtual vector<float> render(size_t count, float freq_mult) = 0;

  void off() {
    // TODO: for now we use a constant release time of 1/5 second; we probably
    // should get this from the AAF somewhere but I don't know where
    this->note_off_decay_remaining = this->note_off_decay_total;
  }

  bool off_complete() const {
    return (this->note_off_decay_remaining == 0);
  }

  float advance_note_off_factor() {
    if (!this->decay_when_off) {
      return 1.0f;
    }
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
  shared_ptr<Channel> channel;
  bool decay_when_off;
  ssize_t note_off_decay_total;
  ssize_t note_off_decay_remaining;
};

class SilentVoice : public Voice {
public:
  SilentVoice(size_t sample_rate, int8_t note, int8_t vel,
      shared_ptr<Channel> channel) : Voice(sample_rate, note, vel, true, channel) { }
  virtual ~SilentVoice() = default;

  virtual vector<float> render(size_t count, float freq_mult) {
    this->advance_note_off_factor();
    return vector<float>(count * 2, 0.0f);
  }
};

class SineVoice : public Voice {
public:
  SineVoice(size_t sample_rate, int8_t note, int8_t vel,
      shared_ptr<Channel> channel) : Voice(sample_rate, note, vel, true, channel),
      offset(0) { }
  virtual ~SineVoice() = default;

  virtual vector<float> render(size_t count, float freq_mult) {
    // TODO: implement pitch bend and freq_mult somehow
    vector<float> data(count * 2, 0.0f);

    double frequency = frequency_for_note(this->note);
    float vel_factor = static_cast<float>(this->vel) / 0x7F;
    for (size_t x = 0; x < count; x++) {
      // panning is 0.0f (left) - 1.0f (right)
      float off_factor = this->advance_note_off_factor();
      data[2 * x + 0] = vel_factor * off_factor * (1.0f - this->channel->panning) * this->channel->volume * sin((2.0f * M_PI * frequency) / this->sample_rate * (x + this->offset));
      data[2 * x + 1] = vel_factor * off_factor * this->channel->panning * this->channel->volume * sin((2.0f * M_PI * frequency) / this->sample_rate * (x + this->offset));
    }
    this->offset += count;

    return data;
  }

  size_t offset;
};

class SampleVoice : public Voice {
public:
  SampleVoice(size_t sample_rate, shared_ptr<const SoundEnvironment> env,
      shared_ptr<SampleCache<const Sound*>> cache, uint16_t bank_id, uint16_t instrument_id,
      int8_t note, int8_t vel, bool decay_when_off, shared_ptr<Channel> channel) :
      Voice(sample_rate, note, vel, decay_when_off, channel),
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
      float pitch_bend_semitone_range, float freq_mult) {
    // stretch it out by the sample rate difference
    float sample_rate_factor = static_cast<float>(sample_rate) /
        static_cast<float>(this->vel_region->sound->sample_rate);

    // compress it so it's the right note
    int8_t base_note = this->vel_region->base_note;
    if (base_note < 0) {
      base_note = this->vel_region->sound->base_note;
    }
    float note_factor = this->vel_region->constant_pitch ? 1.0 :
        (frequency_for_note(base_note) / frequency_for_note(this->note));

    {
      float pitch_bend_factor = pow(2, (pitch_bend * pitch_bend_semitone_range) / 12.0) * freq_mult;
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
      const auto& ret = this->cache->resample_add(
          this->vel_region->sound, this->vel_region->sound->samples(),
          this->vel_region->sound->num_channels, this->src_ratio);
      if (debug_flags & DebugFlag::ShowResampleEvents) {
        string key_low_str = name_for_note(this->key_region->key_low);
        string key_high_str = name_for_note(this->key_region->key_high);
        fprintf(stderr, "[%s:%" PRIX64 "] resampled note %02hhX in range "
            "[%02hhX,%02hhX] [%s,%s] (base %02hhX from %s) (%g), with freq_mult %g, from "
            "%zuHz to %zuHz (%g) with loop at [%zu,%zu]->[%zu,%zu] for an overall "
            "ratio of %g; %zu samples were converted to %zu samples\n",
            this->vel_region->sound->source_filename.c_str(),
            this->vel_region->sound->sound_id,
            this->note,
            this->key_region->key_low,
            this->key_region->key_high,
            key_low_str.c_str(),
            key_high_str.c_str(),
            base_note,
            (this->vel_region->base_note == -1) ? "sample" : "vel region",
            note_factor,
            this->vel_region->freq_mult,
            this->vel_region->sound->sample_rate,
            this->sample_rate,
            sample_rate_factor,
            this->vel_region->sound->loop_start,
            this->vel_region->sound->loop_end,
            this->loop_start_offset,
            this->loop_end_offset,
            this->src_ratio,
            this->vel_region->sound->samples().size(),
            ret.size());
      }
      return ret;
    }
  }

  virtual vector<float> render(size_t count, float freq_mult) {
    vector<float> data(count * 2, 0.0f);

    const auto& samples = this->get_samples(this->channel->pitch_bend,
        this->channel->pitch_bend_semitone_range, freq_mult);
    float vel_factor = static_cast<float>(this->vel) / 0x7F;
    for (size_t x = 0; (x < count) && (this->offset < samples.size()); x++) {
      float off_factor = this->advance_note_off_factor();
      data[2 * x + 0] = vel_factor * off_factor * (1.0f - this->channel->panning) * this->channel->volume * samples[this->offset];
      data[2 * x + 1] = vel_factor * off_factor * this->channel->panning * this->channel->volume * samples[this->offset];

      this->offset++;
      if ((this->note_off_decay_remaining < 0) && (this->loop_end_offset > 0) && (this->offset > this->loop_end_offset)) {
        this->offset = this->loop_start_offset;
      }
    }

    if (this->offset == samples.size()) {
      this->note_off_decay_remaining = 0;
    }

    // apply instrument volume factor
    if (this->vel_region->volume_mult != 1) {
      for (float& s : data) {
        s *= this->vel_region->volume_mult;
      }
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

  shared_ptr<SampleCache<const Sound*>> cache;
};



class Renderer {
protected:
  struct Track {
    int16_t id;
    StringReader r;
    bool reading_wait_opcode; // only used for midi
    uint8_t midi_status; // only used for midi

    unordered_map<size_t, shared_ptr<Channel>> channels;

    float freq_mult;

    int32_t bank; // technically uint16, but uninitialized as -1
    int32_t instrument; // technically uint16, but uninitialized as -1

    unordered_map<size_t, shared_ptr<Voice>> voices;
    unordered_set<shared_ptr<Voice>> voices_off;
    vector<uint32_t> call_stack;

    unordered_map<uint8_t, int16_t> registers;

    Track(int16_t id, shared_ptr<string> data, size_t start_offset, uint32_t bank = -1) :
        id(id), r(data, start_offset), reading_wait_opcode(true), freq_mult(1),
        bank(bank), instrument(-1) { }

    void attenuate_perf() {
      for (auto& channel_it : this->channels) {
        channel_it.second->attenuate();
      }
    }

    void voice_off(size_t voice_id) {
      // some tracks do voice_off for nonexistent voices because of bad looping;
      // just do nothing in that case
      auto v_it = this->voices.find(voice_id);
      if (v_it != this->voices.end()) {
        v_it->second->off();
        this->voices_off.emplace(move(v_it->second));
        this->voices.erase(v_it);
      }
    }

    shared_ptr<Channel> channel(size_t id) {
      auto it = this->channels.find(id);
      if (it != this->channels.end()) {
        return it->second;
      }
      return this->channels.emplace(id, new Channel()).first->second;
    }
  };

  string output_data;
  unordered_set<shared_ptr<Track>> tracks;
  multimap<uint64_t, shared_ptr<Track>> next_event_to_track;

  size_t sample_rate;
  uint64_t current_time;
  size_t samples_rendered;
  uint16_t tempo;
  uint16_t pulse_rate;
  double tempo_bias;

  shared_ptr<const SoundEnvironment> env;
  unordered_set<int16_t> mute_tracks;
  unordered_set<int16_t> solo_tracks;
  unordered_set<int16_t> disable_tracks;
  bool decay_when_off;

  shared_ptr<SampleCache<const Sound*>> cache;

  virtual void execute_opcode(multimap<uint64_t, shared_ptr<Track>>::iterator track_it) = 0;

  void voice_on(shared_ptr<Track> t, size_t voice_id, uint8_t key, uint8_t vel,
      size_t channel_id) {
    shared_ptr<Channel> c = t->channel(channel_id);

    if (this->env) {
      try {
        SampleVoice* v = new SampleVoice(this->sample_rate, this->env,
            this->cache, t->bank, t->instrument, key, vel, this->decay_when_off, c);
        t->voices[voice_id].reset(v);
      } catch (const out_of_range& e) {
        string key_str = name_for_note(key);
        if (debug_flags & DebugFlag::ShowMissingNotes) {
          fprintf(stderr, "warning: can\'t find sample (%s): bank=%" PRIX32
              " instrument=%" PRIX32 " key=%02hhX=%s vel=%02hhX\n", e.what(),
              t->bank, t->instrument, key, key_str.c_str(), vel);
        }
        if (debug_flags & DebugFlag::PlayMissingNotes) {
          t->voices[voice_id].reset(new SineVoice(this->sample_rate, key, vel, c));
        } else {
          t->voices[voice_id].reset(new SilentVoice(this->sample_rate, key, vel, c));
        }
      }
    } else {
      t->voices[voice_id].reset(new SineVoice(this->sample_rate, key, vel, c));
    }
  }

public:
  explicit Renderer(
      size_t sample_rate,
      int resample_method,
      shared_ptr<const SoundEnvironment> env,
      const unordered_set<int16_t>& mute_tracks,
      const unordered_set<int16_t>& solo_tracks,
      const unordered_set<int16_t>& disable_tracks,
      double tempo_bias,
      bool decay_when_off)
    : sample_rate(sample_rate),
      current_time(0),
      samples_rendered(0),
      tempo(0),
      pulse_rate(0),
      tempo_bias(tempo_bias),
      env(env),
      mute_tracks(mute_tracks),
      solo_tracks(solo_tracks),
      disable_tracks(disable_tracks),
      decay_when_off(decay_when_off),
      cache(new SampleCache<const Sound*>(resample_method)) { }

  virtual ~Renderer() = default;

  bool can_render() const {
    // if there are pending opcodes, we can continue rendering
    if (!this->next_event_to_track.empty()) {
      return true;
    }

    // if there are voices waiting to produce sound, we can continue rendering
    for (const auto& t : this->tracks) {
      if (!t->voices.empty() || !t->voices_off.empty()) {
        return true;
      }
    }

    // if neither of the above, we're done
    return false;
  }

  vector<float> render_time_step(size_t queued_buffer_count = 0, size_t buffer_count = 0) {
    // run all opcodes that should execute on the current time step
    while (!this->next_event_to_track.empty() &&
           (current_time == this->next_event_to_track.begin()->first)) {
      auto t_it = this->next_event_to_track.begin();
      size_t offset = t_it->second->r.where();
      try {
        this->execute_opcode(t_it);
      } catch (...) {
        fprintf(stderr, "error at offset %zX\n", offset);
        throw;
      }
    }

    // if all tracks have terminated, turn all of their voices off
    if (this->next_event_to_track.empty()) {
      for (auto& t : this->tracks) {
        unordered_set<uint8_t> voices_on;
        for (const auto& voice_it : t->voices) {
          voices_on.emplace(voice_it.first);
        }
        for (uint8_t voice_id : voices_on) {
          t->voice_off(voice_id);
        }
      }
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
    for (const auto& t : this->tracks) {
      // get all voices, including those that are fading
      unordered_set<shared_ptr<Voice>> all_voices = t->voices_off;
      for (auto& it : t->voices) {
        all_voices.insert(it.second);
      }

      // render all the voices
      for (auto v : all_voices) {
        vector<float> voice_samples;
        try {
          voice_samples = v->render(samples_per_pulse, t->freq_mult);
        } catch (...) {
          fprintf(stderr, "error while rendering voices for track %hd (freq_mult=%g)\n",
              t->id, t->freq_mult);
          throw;
        }
        if (voice_samples.size() != step_samples.size()) {
          throw logic_error(string_printf(
              "voice produced incorrect sample count (returned %zu samples, expected %zu samples)",
              voice_samples.size(), step_samples.size()));
        }
        if (!this->mute_tracks.count(t->id)) {
          for (size_t y = 0; y < voice_samples.size(); y++) {
            step_samples[y] += voice_samples[y];
          }
        }

        // only draw the note in the text view if it's on
        if ((v->note_off_decay_remaining < 0) && (v->note >= 0)) {
          char track_char;
          if (t->id < 0) {
            track_char = '/';
          } else if (t->id < 10) {
            track_char = '0' + t->id;
          } else if (t->id < 36) {
            track_char = 'A' + (t->id - 10);
          } else if (t->id < 62) {
            track_char = 'a' + (t->id - 36);
          } else {
            track_char = '&';
          }
          if (notes_table[v->note] == ' ') {
            notes_table[v->note] = track_char;
          } else if (notes_table[v->note] != track_char) {
            notes_table[v->note] = '+';
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
      if (buffer_count == 0) {
        buffers_color = &white;
      } else if (queued_buffer_count > (2 * buffer_count) / 3) {
        buffers_color = &green;
      } else if (queued_buffer_count > buffer_count / 3) {
        buffers_color = &yellow;
      } else {
        buffers_color = &red;
      }

      bool short_status = !(debug_flags & DebugFlag::ShowLongStatus);
      bool all_tracks_finished = this->next_event_to_track.empty();
      char when_str[16];
      snprintf(when_str, 16, "%-7g", when);

      if ((debug_flags & DebugFlag::ColorField) || (short_status && (debug_flags & DebugFlag::ColorStatus))) {
        fprintf(stderr, "\r%08" PRIX64 "%c %s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.12s%s%.8s%s @ %.7s + %s%zu/%zu%s%c",
            current_time, all_tracks_finished ? '-' : ':',
            field_magenta.c_str(), &notes_table[0], field_red.c_str(), &notes_table[12],
            field_yellow.c_str(), &notes_table[24], field_green.c_str(), &notes_table[36],
            field_cyan.c_str(), &notes_table[48], field_blue.c_str(), &notes_table[60],
            field_magenta.c_str(), &notes_table[72], field_red.c_str(), &notes_table[84],
            field_yellow.c_str(), &notes_table[96], field_green.c_str(), &notes_table[108],
            field_cyan.c_str(), &notes_table[120], white.c_str(), when_str,
            buffers_color->c_str(), queued_buffer_count, buffer_count, white.c_str(),
            short_status ? ' ' : '\n');
      } else if (debug_flags & DebugFlag::ColorStatus) {
        fprintf(stderr, "\r%08" PRIX64 "%c %s @ %.7s + %s%zu/%zu%s%c", current_time,
            all_tracks_finished ? '-' : ':', notes_table, when_str,
            buffers_color->c_str(), queued_buffer_count, buffer_count,
            white.c_str(), short_status ? ' ' : '\n');
      } else {
        fprintf(stderr, "\r%08" PRIX64 "%c %s @ %.7s + %zu/%zu%c", current_time,
            all_tracks_finished ? '-' : ':', notes_table, when_str,
            queued_buffer_count, buffer_count, short_status ? ' ' : '\n');
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
    while (this->can_render() && (this->current_time < time)) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }

  vector<float> render_until_seconds(float seconds) {
    vector<float> samples;
    size_t target_size = seconds * this->sample_rate;
    while (this->can_render() && (this->samples_rendered < target_size)) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }

  vector<float> render_all() {
    vector<float> samples;
    while (this->can_render()) {
      auto step_samples = this->render_time_step();
      samples.insert(samples.end(), step_samples.begin(), step_samples.end());
    }
    return samples;
  }
};

class BMSRenderer : public Renderer {
protected:
  shared_ptr<SequenceProgram> seq;
  shared_ptr<string> seq_data;

public:
  explicit BMSRenderer(
      shared_ptr<SequenceProgram> seq,
      size_t sample_rate,
      int resample_method,
      shared_ptr<const SoundEnvironment> env,
      const unordered_set<int16_t>& mute_tracks,
      const unordered_set<int16_t>& solo_tracks,
      const unordered_set<int16_t>& disable_tracks,
      double tempo_bias,
      bool decay_when_off)
    : Renderer(
        sample_rate,
        resample_method,
        env,
        mute_tracks,
        solo_tracks,
        disable_tracks,
        tempo_bias,
        decay_when_off),
      seq(seq),
      seq_data(new string(seq->data)) {
    shared_ptr<Track> default_track(new Track(-1, this->seq_data, 0, this->seq->index));
    this->tracks.emplace(default_track);
    this->next_event_to_track.emplace(0, default_track);
  }

  virtual ~BMSRenderer() = default;

protected:

  void execute_set_perf(shared_ptr<Track> t, uint8_t type, float value,
      uint16_t duration) {
    shared_ptr<Channel> c = t->channel(0);
    if (duration) {
      if (type == 0x00) {
        c->volume_target = value;
        c->volume_target_frames = duration;
      } else if (type == 0x01) {
        c->pitch_bend_target = value;
        c->pitch_bend_target_frames = duration;
      } else if (type == 0x02) {
        c->reverb_target = value;
        c->reverb_target_frames = duration;
      } else if (type == 0x03) {
        c->panning_target = value;
        c->panning_target_frames = duration;
      } else {
        if (debug_flags & DebugFlag::ShowUnknownPerfOptions) {
          fprintf(stderr, "unknown perf type option: %02hhX (value=%g)\n", type,
              value);
        }
      }

    } else {
      if (type == 0x00) {
        c->volume = value;
        c->volume_target_frames = 0;
      } else if (type == 0x01) {
        c->pitch_bend = value;
        c->pitch_bend_target_frames = 0;
      } else if (type == 0x02) {
        c->reverb = value;
        c->reverb_target_frames = 0;
      } else if (type == 0x03) {
        c->panning = value;
        c->panning_target_frames = 0;
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
      t->channel(0)->pitch_bend_semitone_range = static_cast<float>(value) * 4.0;
    } else {
      if (debug_flags & DebugFlag::ShowUnknownParamOptions) {
        fprintf(stderr, "unknown param type option: %02hhX (value=%hu)\n",
            param, value);
      }
    }
  }

  virtual void execute_opcode(multimap<uint64_t, shared_ptr<Track>>::iterator track_it) {
    shared_ptr<Track> t = track_it->second;

    uint8_t opcode = t->r.get_u8();
    if (opcode < 0x80) {
      // note: opcode is also the note
      uint8_t voice = t->r.get_u8() - 1; // between 1 and 8 inclusive
      uint8_t vel = t->r.get_u8();
      this->voice_on(t, voice, opcode, vel, 0);
      return;
    }

    switch (opcode) {
      case 0x80:
      case 0x88:
      case 0xF0: {
        uint64_t wait_time;
        if (opcode == 0xF0) {
          wait_time = read_variable_int(t->r);
        } else {
          wait_time = (opcode & 0x08) ? t->r.get_u16r() : t->r.get_u8();
        }
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
        t->voice_off(voice);
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

      case 0xE2:
        t->bank = t->r.get_u8();
        break;
      case 0xE3:
        t->instrument = t->r.get_u8();
        break;

      case 0xC1: {
        uint8_t track_id = t->r.get_u8();
        uint32_t offset = t->r.get_u24r();
        if (offset >= t->r.size()) {
          throw invalid_argument(string_printf(
              "cannot start track at pc=0x%" PRIX32 " (from pc=0x%zX)",
              offset, t->r.where() - 5));
        }

        // only start the track if it's not in disable_tracks, and solo_tracks
        // is either not given or contains the track
        if ((this->solo_tracks.empty() || this->solo_tracks.count(track_id)) &&
            !this->disable_tracks.count(track_id)) {
          shared_ptr<Track> new_track(new Track(track_id, this->seq_data, offset, this->seq->index));
          this->tracks.emplace(new_track);
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
        break;
      }

      case 0xFB: { // debug string
        while (t->r.get_u8());
        break;
      }

      case 0xFD: {
        this->pulse_rate = t->r.get_u16r();
        break;
      }

      case 0xE0:
      case 0xFE: {
        this->tempo = t->r.get_u16r() * this->tempo_bias;
        break;
      }

      case 0xFF: {
        // note: we don't delete from this->tracks here because the track can
        // contain voices that are producing sound (Luigi's Mansion does this)
        this->next_event_to_track.erase(track_it);
        break;
      }

      // everything below here are unknown opcodes

      case 0x8C:
      case 0xAE:
      case 0xE1:
      case 0xFA:
      case 0xBF:
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX\n", opcode);
        }
        break;

      case 0xC2:
      case 0xCD:
      case 0xCF:
      case 0xDA:
      case 0xDB:
      case 0xF1:
      case 0xF4:
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%02hhX\n", opcode, t->r.get_u8());
        } else {
          t->r.get_u8();
        }
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
      case 0xE6:
      case 0xF9:
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%04hX\n", opcode, t->r.get_u16r());
        } else {
          t->r.get_u16r();
        }
        break;

      case 0xAD:
      case 0xAF:
      case 0xB9:
      case 0xD6:
      case 0xDD:
      case 0xEF:
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%06X\n", opcode, t->r.get_u24r());
        } else {
          t->r.get_u24r();
        }
        break;

      case 0xA9:
      case 0xAA:
      case 0xDF:
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%08X\n", opcode, t->r.get_u32r());
        } else {
          t->r.get_u32r();
        }
        break;

      case 0xD8: {
        uint8_t reg = t->r.get_u8();
        int16_t value = t->r.get_s16r();
        if (reg == 0x62) {
          this->pulse_rate = value;
        } else if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%02hhX 0x%04hX\n", opcode, reg, value);
        }

        break;
      }

      case 0xB1: {
        uint8_t param1 = t->r.get_u8();
        uint32_t param2 = 0;
        if (param1 == 0x40) {
          param2 = t->r.get_u16r();
        } else if (param1 == 0x80) {
          param2 = t->r.get_u32r();
        }
        if (debug_flags & DebugFlag::ShowUnimplementedOpcodes) {
          fprintf(stderr, "unimplemented opcode: 0x%02hhX 0x%02hhX 0x%08X\n",
              opcode, param1, param2);
        }
        break;
      }

      default:
        throw invalid_argument(string_printf("unknown opcode at offset 0x%zX: 0x%hhX",
            t->r.where() - 1, opcode));
    }
  }
};

class MIDIRenderer : public Renderer {
protected:
  shared_ptr<string> midi_contents;
  bool allow_program_change;
  uint8_t channel_instrument[0x10];

public:
  explicit MIDIRenderer(
      shared_ptr<string> midi_contents,
      size_t sample_rate,
      int resample_method,
      shared_ptr<const SoundEnvironment> env,
      const unordered_set<int16_t>& mute_tracks,
      const unordered_set<int16_t>& solo_tracks,
      const unordered_set<int16_t>& disable_tracks,
      double tempo_bias,
      bool decay_when_off,
      uint8_t percussion_instrument,
      bool allow_program_change)
    : Renderer(
        sample_rate,
        resample_method,
        env,
        mute_tracks,
        solo_tracks,
        disable_tracks,
        tempo_bias,
        decay_when_off),
      midi_contents(midi_contents),
      allow_program_change(allow_program_change) {
    for (uint8_t x = 0; x < 0x10; x++) {
      this->channel_instrument[x] = x;
    }
    if (percussion_instrument) {
      this->channel_instrument[9] = percussion_instrument;
    }

    StringReader r(this->midi_contents);

    // read the header and create all the tracks
    MIDIHeaderChunk header = r.get<MIDIHeaderChunk>();
    header.byteswap();
    if (header.header.magic != 0x4D546864) { // 'MThd'
      throw runtime_error("header identifier is incorrect");
    }
    if (header.header.size < 6) {
      throw runtime_error("header is too small");
    }
    if (header.format > 2) {
      throw runtime_error("MIDI format is unknown");
    }

    // if the header is larger, skip the extra bytes
    if (header.header.size > 6) {
      r.go(r.where() + (header.header.size - sizeof(MIDIHeaderChunk)));
    }

    // create all the tracks
    for (size_t track_id = 0; track_id < header.track_count; track_id++) {
      MIDITrackChunk ch = r.get<MIDITrackChunk>();
      ch.byteswap();
      if (ch.header.magic != 0x4D54726B) {
        throw runtime_error("track header not present");
      }

      if ((this->solo_tracks.empty() || this->solo_tracks.count(track_id)) &&
          !this->disable_tracks.count(track_id)) {
        shared_ptr<Track> t(new Track(track_id, this->midi_contents, r.where(), 0));
        this->tracks.emplace(t);
        this->next_event_to_track.emplace(0, t);
      }

      r.go(r.where() + ch.header.size);
    }

    // set the tempo if it's given in absolute terms
    if (header.division & 0x8000) {
      // TODO: figure out if this logic is right
      uint8_t frames_per_sec = -((header.division >> 8) & 0x7F);
      uint8_t ticks_per_frame = header.division & 0xFF;
      int64_t ticks_per_sec = static_cast<int64_t>(ticks_per_frame) *
          static_cast<int64_t>(frames_per_sec);
      this->tempo = 120 * this->tempo_bias;
      this->pulse_rate = ticks_per_sec / 2;

    } else {
      this->tempo = 120 * this->tempo_bias;
      this->pulse_rate = header.division;
    }
  }

  virtual ~MIDIRenderer() = default;

protected:

  virtual void execute_opcode(multimap<uint64_t, shared_ptr<Track>>::iterator track_it) {
    shared_ptr<Track> t = track_it->second;

    t->reading_wait_opcode = !t->reading_wait_opcode;
    if (!t->reading_wait_opcode) {
      uint32_t wait_time = read_variable_int(t->r);
      if (wait_time) {
        uint64_t reactivation_time = this->current_time + wait_time;
        this->next_event_to_track.erase(track_it);
        this->next_event_to_track.emplace(reactivation_time, t);
      }
      return;
    }

    // if the status byte is omitted, use the status from the previous command
    uint8_t new_status = t->r.get_u8();
    if (new_status & 0x80) {
      t->midi_status = new_status;
    } else {
      t->r.go(t->r.where() - 1);
    }

    if ((t->midi_status & 0xF0) == 0x80) { // note off
      uint8_t channel = t->midi_status & 0x0F;
      uint8_t key = t->r.get_u8();
      t->r.get_u8(); // vel (ignored; see note below)

      // note: simcity midis sometimes have incorrect velocities in note-off
      // commands, so we don't include it in the voice id
      uint32_t voice_id = (static_cast<uint32_t>(channel) << 8) |
          static_cast<uint32_t>(key);
      t->voice_off(voice_id);

    } else if ((t->midi_status & 0xF0) == 0x90) { // note on
      uint8_t channel = t->midi_status & 0x0F;
      t->instrument = this->channel_instrument[channel];
      uint8_t key = t->r.get_u8();
      uint8_t vel = t->r.get_u8();

      uint32_t voice_id = (static_cast<uint32_t>(channel) << 8) |
          static_cast<uint32_t>(key);
      this->voice_on(t, voice_id, key, vel, channel);

    } else if ((t->midi_status & 0xF0) == 0xA0) { // change key pressure
      // uint8_t channel = t->midi_status & 0x0F;
      t->r.get_u8(); // key
      t->r.get_u8(); // vel
      // TODO

    } else if ((t->midi_status & 0xF0) == 0xB0) { // controller change OR channel mode
      uint8_t channel = t->midi_status & 0x0F;
      uint8_t controller = t->r.get_u8();
      uint8_t value = t->r.get_u8();
      if (controller == 0x07) {
        t->channel(channel)->volume_target = static_cast<float>(value) / 0x7F;
        t->channel(channel)->volume = static_cast<float>(value) / 0x7F;
      } else if (controller == 0x0A) {
        t->channel(channel)->panning_target = static_cast<float>(value) / 0x7F;
        t->channel(channel)->panning = static_cast<float>(value) / 0x7F;
      }
      // TODO: implement more controller messages

    } else if ((t->midi_status & 0xF0) == 0xC0) { // program change
      uint8_t channel = t->midi_status & 0x0F;
      uint8_t program = t->r.get_u8();
      if (this->allow_program_change) {
        this->channel_instrument[channel] = program;
      }

    } else if ((t->midi_status & 0xF0) == 0xD0) { // channel key pressure
      // uint8_t channel = t->midi_status & 0x0F;
      t->r.get_u8(); // vel
      // TODO

    } else if ((t->midi_status & 0xF0) == 0xE0) { // pitch bend
      // uint8_t channel = t->midi_status & 0x0F;
      t->r.get_u8(); // lsb
      t->r.get_u8(); // msb
      // uint16_t value = (msb << 7) | lsb; // yes, each is 7 bits, not 8

    } else if (t->midi_status == 0xFF) { // meta event
      uint8_t type = t->r.get_u8();
      uint8_t size = t->r.get_u8();

      if (type == 0x2F) { // end track
        // note: we don't delete from this->tracks here because the track can
        // contain voices that are producing sound (After Dark does this)
        this->next_event_to_track.erase(track_it);

      } else if (type == 0x51) { // set tempo
        this->tempo = (60000000 / t->r.get_u24r()) * this->tempo_bias;

      } else { // anything else? just skip it
        t->r.go(t->r.where() + size);
      }
    }
  }
};



void print_usage(const char* argv0) {
  fprintf(stderr, "\
Usage:\n\
  %s sequence_name [options]\n\
\n\
Input options:\n\
  sequence_name: the name of the sequence. This can be a filename, or if\n\
      --audiores-directory is used, it can also be the name of a sequence\n\
      defined in the environment. If --list is used, no sequence name should\n\
      be given.\n\
  --audiores-directory=dir_name: load environment from this directory. The\n\
      directory should include a file named pikibank.bx, JaiInit.aaf,\n\
      GCKart.baa, or msound.aaf.\n\
  --json-environment=filename.json: load MIDI environment from this JSON file.\n\
      If given, --midi is implied.\n\
  --midi: treat input sequence as MIDI instead of BMS.\n\
  --midi-instrument=N:filename.wav[:base_note]: map MIDI channel N to an\n\
      instrument composed of the given sound, with an optional base note\n\
      (default 0x3C).\n\
\n\
Output options (only one of these may be given):\n\
  --list: list the names of sequences in the loaded environment.\n\
  --disassemble: disassemble the sequence (default).\n"
#ifndef WINDOWS // play not supported on windows
"  --play: play the sequence to the default audio device using OpenAL streaming.\n"
#endif
"  --output-filename=file.wav: write the synthesized audio to this file.\n\
\n\
Synthesis options:\n\
  --disable-track=N: disable track N entirely (can be given multiple times).\n\
  --solo-track=N: disable all tracks except N (can be given multiple times).\n\
      For BMS, the default track (-1) is not disabled by this option.\n\
  --mute-track=N: execute instructions for track N, but mute its sound.\n\
  --midi-tempo-bias=BIAS: for MIDI, play songs at this proportion of their\n\
      original speed.\n\
  --time-limit=N: stop after this many seconds (default 5 minutes).\n"
#ifndef WINDOWS
"      When --play is used, this option is ignored.\n"
#endif
"  --start-time=N: discard this many seconds of audio at the beginning.\n\
  --sample-rate=N: generate output at this sample rate (default 48000).\n\
  --resample-method=METHOD: use this method for resampling waveforms. Values\n\
      are sinc-best, sinc-medium, sinc-fast, hold, and linear. When generating\n\
      a WAV output file, the default method is sinc-best.\n"
#ifndef WINDOWS
"      When playing, the default method is linear.\n\
  --play-buffers=N: generate this many steps of audio in advance of the play\n\
      position (default 128). If play lags, especially during pitch bends, try\n\
      increasing this value.\n"
#endif
"\n\
Logging options:\n\
  --silent: don't print any status information.\n\
  --verbose: print extra debugging events.\n"
#ifndef WINDOWS
"  --no-color: don\'t use terminal escape codes for color in the output.\n"
#endif
"  --short-status: only show one line of status information.\n\
  --long-status: show status history (default unless writing an output file).\n\
\n\
Debugging options:\n\
  --default-bank=N: override automatic instrument bank detection and use bank\n\
      N instead.\n\
  --no-decay-when-off: make note off events only terminate audio loops instead\n\
      of also tapering off the volume of the note.\n\
  --play-missing-notes: for notes that have no associated instrument/sample,\n\
      play a sine wave instead.\n\
", argv0);
}

int main(int argc, char** argv) {

  // default to no color if stderr isn't a tty
  if (!isatty(fileno(stderr))) {
    debug_flags &= ~DebugFlag::AllColorOptions;
  }

  string filename;
  const char* output_filename = NULL;
  const char* aaf_directory = NULL;
  bool midi = false;
  unordered_map<int16_t, InstrumentMetadata> midi_instrument_metadata;
  unordered_set<int16_t> disable_tracks;
  unordered_set<int16_t> mute_tracks;
  unordered_set<int16_t> solo_tracks;
  float time_limit = 300.0f;
  float start_time = 0.0f;
  size_t sample_rate = 48000;
  bool play = false;
  float tempo_bias = 1.0;
#ifndef WINDOWS
  size_t num_buffers = 128;
#endif
  bool list_sequences = false;
  int32_t default_bank = -1;
  bool decay_when_off = true;
  int resample_method = SRC_SINC_BEST_QUALITY;
  bool resample_method_set = false;
  string env_json_filename;
  for (int x = 1; x < argc; x++) {
    if (!strncmp(argv[x], "--disable-track=", 16)) {
      disable_tracks.emplace(atoi(&argv[x][16]));
    } else if (!strncmp(argv[x], "--mute-track=", 13)) {
      mute_tracks.emplace(atoi(&argv[x][13]));
    } else if (!strncmp(argv[x], "--solo-track=", 13)) {
      solo_tracks.emplace(atoi(&argv[x][13]));
    } else if (!strncmp(argv[x], "--time-limit=", 13)) {
      time_limit = atof(&argv[x][13]);
    } else if (!strncmp(argv[x], "--start-time=", 13)) {
      start_time = atof(&argv[x][13]);
    } else if (!strncmp(argv[x], "--sample-rate=", 14)) {
      sample_rate = atoi(&argv[x][14]);
    } else if (!strncmp(argv[x], "--audiores-directory=", 21)) {
      aaf_directory = &argv[x][21];
    } else if (!strncmp(argv[x], "--json-environment=", 19)) {
      env_json_filename = &argv[x][19];
    } else if (!strncmp(argv[x], "--output-filename=", 18)) {
      output_filename = &argv[x][18];
      debug_flags &= ~DebugFlag::ShowLongStatus;
    } else if (!strcmp(argv[x], "--no-decay-when-off")) {
      decay_when_off = false;
    } else if (!strcmp(argv[x], "--midi")) {
      midi = true;
    } else if (!strncmp(argv[x], "--midi-channel-instrument=", 26)) {
      auto tokens = split(&argv[x][26], ':');
      if (tokens.size() < 2 || tokens.size() > 3) {
        fprintf(stderr, "invalid argument format: %s\n", argv[x]);
        return 1;
      }
      InstrumentMetadata im;
      int16_t channel_id = stoull(tokens[0], NULL, 0);
      im.filename = tokens[1];
      im.base_note = (tokens.size() > 2) ? stoul(tokens[2], NULL, 0) : -1;
      midi_instrument_metadata.emplace(channel_id, move(im));

    } else if (!strcmp(argv[x], "--verbose")) {
      debug_flags = 0xFFFFFFFFFFFFFFFF;
    } else if (!strncmp(argv[x], "--debug-flags=", 14)) {
      debug_flags = atoi(&argv[x][14]);
#ifndef WINDOWS
    } else if (!strcmp(argv[x], "--no-color")) {
      debug_flags &= ~DebugFlag::AllColorOptions;
#endif
    } else if (!strcmp(argv[x], "--short-status")) {
      debug_flags &= ~DebugFlag::ShowLongStatus;
    } else if (!strcmp(argv[x], "--long-status")) {
      debug_flags |= DebugFlag::ShowLongStatus;
    } else if (!strcmp(argv[x], "--play-missing-notes")) {
      debug_flags |= DebugFlag::PlayMissingNotes;
    } else if (!strcmp(argv[x], "--quiet")) {
      debug_flags = 0;

    } else if (!strcmp(argv[x], "--resample-method=sinc-best")) {
      resample_method = SRC_SINC_BEST_QUALITY;
      resample_method_set = true;
    } else if (!strcmp(argv[x], "--resample-method=sinc-medium")) {
      resample_method = SRC_SINC_MEDIUM_QUALITY;
      resample_method_set = true;
    } else if (!strcmp(argv[x], "--resample-method=sinc-fast")) {
      resample_method = SRC_SINC_FASTEST;
      resample_method_set = true;
    } else if (!strcmp(argv[x], "--resample-method=hold")) {
      resample_method = SRC_ZERO_ORDER_HOLD;
      resample_method_set = true;
    } else if (!strcmp(argv[x], "--resample-method=linear")) {
      resample_method = SRC_LINEAR;
      resample_method_set = true;

    } else if (!strncmp(argv[x], "--default-bank=", 15)) {
      default_bank = atoi(&argv[x][15]);
    } else if (!strncmp(argv[x], "--tempo-bias=", 13)) {
      tempo_bias = atof(&argv[x][13]);
#ifndef WINDOWS
    } else if (!strncmp(argv[x], "--play-buffers=", 15)) {
      num_buffers = atoi(&argv[x][15]);
    } else if (!strcmp(argv[x], "--play")) {
      play = true;
      if (!resample_method_set) {
        resample_method = SRC_LINEAR;
      }
#endif
    } else if (!strcmp(argv[x], "--disassemble")) {
      play = false;
      list_sequences = false;
    } else if (!strcmp(argv[x], "--list")) {
      list_sequences = true;
    } else if (filename.empty()) {
      filename = argv[x];
    } else {
      throw invalid_argument("too many positional command-line args");
    }
  }

#ifdef WINDOWS
  (void)resample_method_set; // suppress warning about unused variable
#endif

  shared_ptr<JSONObject> env_json;
  string env_json_dir;
  if (!env_json_filename.empty()) {
    env_json = JSONObject::parse(load_file(env_json_filename));

    size_t slash_pos = env_json_filename.rfind('/');
    if (slash_pos == string::npos) {
      env_json_dir = ".";
    } else {
      env_json_dir = env_json_filename.substr(0, slash_pos);
    }

    if (filename.empty()) {
      filename = env_json_dir + "/" + env_json->as_dict().at("sequence_filename")->as_string();
    }
    if (env_json->as_dict().at("sequence_type")->as_string() != "MIDI") {
      fprintf(stderr, "JSON environments may only contain MIDI sequences\n");
      return 1;
    }
    midi = true;
  }

  if (filename.empty() && !list_sequences) {
    print_usage(argv[0]);
    throw invalid_argument("no filename given");
  }

  // load the sound environment from the AAF, the CLI, or the JSON
  shared_ptr<const SoundEnvironment> env;
  if (env_json.get()) {
    env.reset(new SoundEnvironment(create_json_sound_environment(
        env_json->as_dict().at("instruments"), env_json_dir)));
  } else if (aaf_directory) {
    env.reset(new SoundEnvironment(load_sound_environment(aaf_directory)));
  } else if (midi) {
    env.reset(new SoundEnvironment(create_midi_sound_environment(
        midi_instrument_metadata)));
  }

  if (list_sequences) {
    if (env->sequence_programs.empty()) {
      fprintf(stdout, "there are no sequences in the environment\n");
      return 0;
    }
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
    return 0;
  }

  // for bms, try to get the sequence from the env if it's there
  // for midi, load the contents of the midi file
  shared_ptr<SequenceProgram> seq;
  shared_ptr<string> midi_contents;
  if (midi) {
    midi_contents.reset(new string(load_file(filename)));

  } else {
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
            filename.c_str());
        return 2;
      }
    }
  }

  if (default_bank >= 0) {
    seq->index = default_bank;
  }

  // if not playing and not writing to a file, disassemble
  if (!output_filename && !play) {
    if (midi) {
      StringReader r(midi_contents);
      disassemble_midi(r);
    } else {
      shared_ptr<string> seq_data(new string(seq->data));
      StringReader r(seq_data);
      disassemble_bms(r, seq->index);
    }
    return 0;
  }

  shared_ptr<Renderer> r;
  if (seq.get()) {
    r.reset(new BMSRenderer(
        seq,
        sample_rate,
        resample_method,
        env,
        mute_tracks,
        solo_tracks,
        disable_tracks,
        tempo_bias,
        decay_when_off));
  } else {
    // midi has some extra params; get them from the json if possible
    uint8_t percussion_instrument = 0;
    bool allow_program_change = true;
    if (env_json.get()) {
      try {
        percussion_instrument = env_json->as_dict().at("percussion_instrument")->as_int();
      } catch (const out_of_range&) { }
      try {
        allow_program_change = env_json->as_dict().at("allow_program_change")->as_bool();
      } catch (const out_of_range&) { }
      try {
        tempo_bias *= env_json->as_dict().at("tempo_bias")->as_float();
      } catch (const out_of_range&) { }
    }
    r.reset(new MIDIRenderer(
        midi_contents,
        sample_rate,
        resample_method,
        env,
        mute_tracks,
        solo_tracks,
        disable_tracks,
        tempo_bias,
        decay_when_off,
        percussion_instrument,
        allow_program_change));
  }

  // skip the first bit if requested
  if (start_time) {
    r->render_until_seconds(start_time);
  }

  if (output_filename) {
    auto samples = r->render_until_seconds(time_limit);
    fprintf(stderr, "\nsaving output file: %s\n", output_filename);
    save_wav(output_filename, samples, sample_rate, 2);

#ifndef WINDOWS
  } else if (play) {
    init_al();
    AudioStream stream(sample_rate, AL_FORMAT_STEREO16, num_buffers);
    for (;;) {
      stream.check_buffers();
      if (!r->can_render()) {
        break;
      }
      auto step_samples = r->render_time_step(stream.queued_buffer_count(),
          stream.buffer_count());
      vector<int16_t> al_samples = convert_samples_f32_to_s16(step_samples);
      stream.add_samples(al_samples.data(), al_samples.size() / 2);
    }
    if (debug_flags & DebugFlag::ShowNotesOn) {
      fprintf(stderr, "\nrendering complete; waiting for buffers to drain\n");
    }
    stream.wait();
    exit_al();
#endif
  }

  return 0;
}
