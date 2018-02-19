#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>
#include <string>

#include "instrument.hh"

using namespace std;



struct SequenceProgram {
  uint32_t index;
  std::string data;

  SequenceProgram(uint32_t index, std::string&& data);
};

struct SoundEnvironment {
  unordered_map<uint32_t, InstrumentBank> instrument_banks;
  vector<vector<Sound>> sample_banks;
  unordered_map<string, SequenceProgram> sequence_programs;

  void resolve_pointers();
};

SoundEnvironment load_sound_environment(const char* aw_directory);
