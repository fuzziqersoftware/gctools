#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/JSON.hh>
#include <phosg/Strings.hh>
#include <vector>
#include <string>

#include "instrument.hh"



struct SequenceProgram {
  uint32_t index;
  std::string data;

  SequenceProgram(uint32_t index, std::string&& data);
};

struct SoundEnvironment {
  std::unordered_map<uint32_t, InstrumentBank> instrument_banks;
  std::unordered_map<uint32_t, std::vector<Sound>> sample_banks;
  std::unordered_map<std::string, SequenceProgram> sequence_programs;

  void resolve_pointers();
  void merge_from(SoundEnvironment&& other);
};

struct InstrumentMetadata {
  std::string filename;
  int16_t base_note;
};

SoundEnvironment load_sound_environment(const char* aw_directory);
SoundEnvironment create_midi_sound_environment(
    const std::unordered_map<int16_t, InstrumentMetadata>& instrument_metadata);
SoundEnvironment create_json_sound_environment(
    std::shared_ptr<const JSONObject> instruments_json,
    const std::string& directory);
