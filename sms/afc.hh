#pragma once

#include <inttypes.h>

#include <vector>

std::vector<int16_t> afc_decode(const void* data, size_t size,
    bool small_frames);
