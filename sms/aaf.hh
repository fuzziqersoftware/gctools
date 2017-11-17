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



std::vector<Sound> wsys_decode(void* vdata, size_t size,
    const char* aw_directory);

struct SoundEnvironment {
  unordered_map<uint32_t, InstrumentBank> instrument_banks;
  vector<vector<Sound>> sample_banks;
};

SoundEnvironment aaf_decode(void* vdata, size_t size, const char* aw_directory);
SoundEnvironment aaf_decode_directory(const char* aw_directory);
