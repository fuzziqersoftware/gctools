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



struct SoundEnvironment {
  unordered_map<uint32_t, InstrumentBank> instrument_banks;
  vector<vector<Sound>> sample_banks;
  unordered_map<string, string> sequence_programs;
};

SoundEnvironment aaf_decode(void* vdata, size_t size, const char* aw_directory);
SoundEnvironment aaf_decode_directory(const char* aw_directory);
